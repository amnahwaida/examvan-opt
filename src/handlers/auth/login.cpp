#include "handlers/auth/login.hpp"
#include "session/csrf.hpp"
#include "session/cookie.hpp"
#include "middleware/turnstile.hpp"
#include "helpers/utils.hpp"
#include <openssl/rand.h>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <crypt.h>
#include <cctype>
#include <algorithm>
#ifdef HAS_LIBPQ
#include "db/pool_real.hpp"
#include "db/pool.hpp"
#endif

namespace examvan::handlers::auth {

static std::unordered_map<std::string, std::string> g_users;
static std::mutex g_mu;
static std::string gensalt(){ // bcrypt gensalt
  unsigned char buf[16];
  if(RAND_bytes(buf,sizeof(buf))!=1){ throw std::runtime_error("RAND_bytes failed"); }
  const char* b64="./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::string s;
  s.reserve(22);
  int v=0, bits=0;
  for(int i=0;i<16;i++){
    v = (v<<8)|buf[i];
    bits+=8;
    while(bits>=6){ bits-=6; s.push_back(b64[(v>>bits)&0x3F]); }
  }
  if(bits>0) s.push_back(b64[(v<<(6-bits))&0x3F]);
  if(s.size()>22) s.resize(22);
  return s;
}
static std::string hash_password(const std::string& p){
  std::string salt="$2b$12$"+gensalt();
  struct crypt_data cd{}; cd.initialized=0;
  char* out=crypt_r(p.c_str(), salt.c_str(), &cd);
  if(out) return std::string(out);
  throw std::runtime_error("crypt_r failed");
}
static bool verify_password(const std::string& plain, const std::string& hashed){
  if(hashed.rfind("$2b$",0)==0 || hashed.rfind("$2a$",0)==0 || hashed.rfind("$2y$",0)==0){
    struct crypt_data cd{}; cd.initialized=0;
    char* out=crypt_r(plain.c_str(), hashed.c_str(), &cd);
    if(!out) return false;
    if(std::string(out).size()!=hashed.size()) return false;
    volatile int d=0;
    for(size_t i=0;i<hashed.size();i++) d|=out[i]^hashed[i];
    return d==0;
  }
  return false;
}
void set_user_for_test(const std::string& u, const std::string& p, const std::string& r){
  (void)r; std::lock_guard<std::mutex> g(g_mu); g_users[u]=hash_password(p);
}
void clear_users_for_test(){ std::lock_guard<std::mutex> g(g_mu); g_users.clear(); }
std::string get_csrf_for_test(const std::string& ck){ (void)ck; return "test-csrf-token"; }

Response login_page(const Request&){
  std::string csrf=generate_csrf_token();
  std::ifstream fr("templates/admin/login.rendered.html");
  if(!fr) fr.open("templates/public/login.rendered.html");
  if(!fr) fr.open("templates/admin/login.html");
  if(fr){
    std::ostringstream ss; ss<<fr.rdbuf();
    std::string html=ss.str();
    auto replace_all=[&](const std::string& from){
      size_t p=0; while((p=html.find(from,p))!=std::string::npos){ html.replace(p,from.size(),csrf); p+=csrf.size(); }
    };
    replace_all("{{.csrf_token}}");
    replace_all("{{ .csrf_token }}");
    replace_all("{{.csrf_token }}");
    replace_all("{{ .csrf_token}}");
    replace_all("CSRF_PLACEHOLDER");
    auto replace_attr=[&](const std::string& needle){
      size_t pos=0;
      while((pos=html.find(needle,pos))!=std::string::npos){
        size_t q1=html.find('"',pos+needle.size()-1);
        if(q1==std::string::npos) q1=html.find('\'',pos+needle.size()-1);
        if(q1==std::string::npos) break;
        char qc=html[q1];
        size_t q2=html.find(qc,q1+1);
        if(q2==std::string::npos) break;
        html.replace(q1+1,q2-q1-1,csrf);
        pos=q2+1;
      }
    };
    replace_attr("csrf-token\" content=\"");
    replace_attr("csrf_token\" value=\"");
    replace_attr("_csrf\" value=\"");
    replace_attr("csrf-token' content='");
    replace_attr("csrf_token' value='");
    std::string ck="csrf_token="+csrf+"; Path=/; SameSite=Lax";
    if(!Config::load().is_development()) ck+="; Secure";
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=ck;
    r.body=html; return r;
  }
  std::string ck2="csrf_token="+csrf+"; Path=/; SameSite=Lax";
  if(!Config::load().is_development()) ck2+="; Secure";
  Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=ck2;
  r.body="<html><body><form method=\"POST\" action=\"/login\"><input name=\"username\"><input name=\"password\" type=\"password\"><input type=\"hidden\" name=\"_csrf\" value=\""+csrf+"\"><button>Login</button></form></body></html>";
  return r;
}

static std::string get_hdr_ci(const Request& req, const std::string& name){
  for(auto &kv: req.headers){
    if(kv.first.size()!=name.size()) continue;
    bool eq=true; for(size_t i=0;i<name.size();i++) if(tolower((unsigned char)kv.first[i])!=tolower((unsigned char)name[i])){eq=false;break;}
    if(eq) return kv.second;
  }
  return "";
}
static std::string json_field(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t n=body.size(); bool in_str=false; bool esc=false;
  for(size_t i=0;i<n;){
    if(!in_str && !esc && i+needle.size()<=n && body.compare(i,needle.size(),needle)==0){
      size_t c=i+needle.size(); while(c<n && (body[c]==' '||body[c]=='\t'||body[c]=='\n'||body[c]=='\r')) c++;
      if(c<n && body[c]==':'){ size_t v=c+1; while(v<n && (body[v]==' '||body[v]=='\t'||body[v]=='\n'||body[v]=='\r')) v++;
        if(v<n && body[v]=='"'){ size_t q=v; size_t e=q+1; while(e<n){ if(body[e]=='\\'){e+=2;continue;} if(body[e]=='"') break; e++; } if(e<n) return body.substr(q+1,e-q-1); }
        else if(v<n){ size_t e=v; while(e<n && body[e]!=',' && body[e]!='}' && body[e]!='"') e++; std::string val=body.substr(v,e-v); size_t a=val.find_first_not_of(" \t\r\n"); size_t b=val.find_last_not_of(" \t\r\n"); if(a!=std::string::npos) val=val.substr(a,b-a+1); return val; }
      }
    }
    char ch=body[i]; if(esc) esc=false; else if(ch=='\\' && in_str) esc=true; else if(ch=='"') in_str=!in_str; i++;
  }
  return "";
}
Response login_handler(const Request& req, const Config& cfg){
  auto form=helpers::parse_form(req.body);
  std::string csrf_header=get_hdr_ci(req,"X-CSRF-Token");
  if(csrf_header.empty()) csrf_header=get_hdr_ci(req,"X-XSRF-Token");
  if(csrf_header.empty()){
    auto f=form.find("csrf_token"); if(f==form.end()) f=form.find("_csrf"); if(f==form.end()) f=form.find("csrf"); if(f!=form.end()) csrf_header=f->second;
    if(csrf_header.empty()) csrf_header=json_field(req.body,"csrf_token");
    if(csrf_header.empty()) csrf_header=json_field(req.body,"_csrf");
    if(csrf_header.empty()) csrf_header=json_field(req.body,"csrf");
    if(csrf_header.empty()) csrf_header=json_field(req.body,"x-csrf-token");
  }
  std::string session_csrf;
  std::string cookie_hdr=get_hdr_ci(req,"Cookie");
  if(!cookie_hdr.empty()){ auto c=extract_cookie(cookie_hdr,"csrf_token"); if(!c.empty()) session_csrf=c; }
  if(session_csrf.empty()){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}"); return r;
  }
  if(!verify_csrf(session_csrf, csrf_header)){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}"); return r;
  }
  std::string turnstile;
  auto tf=form.find("cf-turnstile-response"); if(tf!=form.end()) turnstile=tf->second;
  if(turnstile.empty()) turnstile=json_field(req.body,"cf-turnstile-response");
  if(!turnstile.empty()){
    std::string secret = cfg.turnstile_secret;
    if(secret.empty()) if(auto* e=getenv("TURNSTILE_SECRET")) secret=e;
    if(!middleware::verify_turnstile(turnstile, secret, "")){
      Response r; r.status=403; r.json(403,"{\"error\":\"Turnstile failed\"}"); return r;
    }
  }
  std::string username, password;
  if(auto u=form.find("username"); u!=form.end()) username=u->second;
  if(username.empty()) username=json_field(req.body,"username");
  if(username.empty()) username=json_field(req.body,"email");
  if(auto eu=form.find("email"); eu!=form.end() && username.empty()) username=eu->second;
  if(auto p=form.find("password"); p!=form.end()) password=p->second;
  if(password.empty()) password=json_field(req.body,"password");
  if(username.empty()||password.empty()){
    Response r; r.status=400; r.json(400,"{\"error\":\"username and password required\"}"); return r;
  }
  auto trim_lc=[](std::string s){
    size_t a=s.find_first_not_of(" \t\r\n"); if(a==std::string::npos) return std::string();
    size_t b=s.find_last_not_of(" \t\r\n"); s=s.substr(a,b-a+1);
    for(char &c: s) c=tolower((unsigned char)c);
    return s;
  };
  std::string uname_norm=trim_lc(username);
  std::string stored;
  { std::lock_guard<std::mutex> g(g_mu); 
    auto f=g_users.find(username); if(f!=g_users.end()) stored=f->second;
    else { auto f2=g_users.find(uname_norm); if(f2!=g_users.end()) stored=f2->second; }
  }
  bool ok = !stored.empty() && verify_password(password, stored);
  if(!ok){
#ifdef HAS_LIBPQ
    try{
      std::string db_url=cfg.database_url;
      if(db_url.empty()) if(auto* e=getenv("DATABASE_URL")) db_url=e;
      if(!db_url.empty()){
        std::string ci=pg_conninfo_from_url(db_url);
        if(ci.empty()) ci=db_url;
        db::RealPool pool(ci, 2);
        if(pool.connect()){
          if(auto c=pool.acquire()){
            auto res=pool.exec_params(c.get(),"SELECT password_hash FROM admin_users WHERE lower(username)=lower($1) LIMIT 1",{uname_norm});
            if(res && PQntuples(res.get())>0){
              std::string db_hash=PQgetvalue(res.get(),0,0);
              if(verify_password(password, db_hash)){
                ok=true;
                stored=db_hash;
              }
            }
          }
        }
      }
    }catch(...){}
#endif
  }
  if(!ok){
    Response r; r.status=401; r.json(401,"{\"error\":\"invalid credentials\"}"); return r;
  }
  username=uname_norm;
  std::string payload=b64_encode("admin_id=1&username="+username+"&role=[\"guru\"]");
  std::string cookie="examvan_session="+encode_cookie_value(cfg.secret_key, payload)+"; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400";
  if(!cfg.is_development()) cookie+="; Secure";
  /* Klien API (fetch/AJAX) tetap menerima JSON {success,message} sesuai kontrak
   * F1 §5. Form HTML biasa tidak punya header tersebut → 303 redirect agar
   * browser langsung menuju dashboard/next, bukan menampilkan JSON mentah. */
  bool wants_json=false;
  if(get_hdr_ci(req,"Accept").find("application/json")!=std::string::npos) wants_json=true;
  if(get_hdr_ci(req,"X-Requested-With")=="XMLHttpRequest") wants_json=true;
  if(!wants_json){ std::string ct=get_hdr_ci(req,"Content-Type"); if(ct.find("application/json")!=std::string::npos) wants_json=true; }
  if(wants_json){
    Response r; r.json(200,"{\"success\":true,\"username\":\""+username+"\"}");
    r.headers["Set-Cookie"]=cookie;
    return r;
  }
  std::string target="/admin/dashboard";
  std::string next_val;
  if(auto n=form.find("next"); n!=form.end() && !n->second.empty()) next_val=n->second;
  else { next_val=json_field(req.body,"next"); }
  if(!next_val.empty()){
    std::string nx = helpers::url_decode(next_val);
    nx = helpers::url_decode(nx);
    if(!nx.empty() && nx[0]=='/' && (nx.size()==1 || nx[1]!='/') && nx.find('\\')==std::string::npos && nx.find("%2f")==std::string::npos && nx.find("%2F")==std::string::npos && nx.find("%5c")==std::string::npos && nx.find("%5C")==std::string::npos && nx.find("//")==std::string::npos && nx.find("..")==std::string::npos && nx.find(':')==std::string::npos) target=nx;
  }
  Response r; r.status=303; r.headers["Location"]=target; r.headers["Set-Cookie"]=cookie;
  return r;
}

} // namespace examvan::handlers::auth
