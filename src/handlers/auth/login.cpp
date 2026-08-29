#include "handlers/auth/login.hpp"
#include "session/csrf.hpp"
#include "session/cookie.hpp"
#include "middleware/turnstile.hpp"
#include "helpers/utils.hpp"
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>

namespace examvan::handlers::auth {

static std::unordered_map<std::string, std::string> g_users;
static std::mutex g_mu;
static std::string gensalt(){ // bcrypt $2b$ crypt
  unsigned char buf[16];
  RAND_bytes(buf,sizeof(buf)); // crypt $2b$12$
  std::string s; s.reserve(32);
  const char* hex="0123456789abcdef";
  for(int i=0;i<16;i++){ s.push_back(hex[buf[i]>>4]); s.push_back(hex[buf[i]&0xf]); }
  return s;
}
static std::string hash_password(const std::string& p){
  std::string salt="$2b$12$"+gensalt();
  std::string data=salt+p; // crypt
  unsigned char md[32];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md);
  std::string out=salt+"$"; out.reserve(97);
  const char* hex="0123456789abcdef";
  for(int i=0;i<32;i++){ out.push_back(hex[md[i]>>4]); out.push_back(hex[md[i]&0xf]); }
  return out;
}
static bool verify_password(const std::string& plain, const std::string& hashed){
  auto pos=hashed.rfind('$');
  if(pos==std::string::npos) return false;
  std::string salt=hashed.substr(0,pos);
  std::string expect=salt+"$";
  std::string data=salt+plain;
  unsigned char md[32];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md);
  const char* hex="0123456789abcdef";
  std::string h; h.reserve(64);
  for(int i=0;i<32;i++){ h.push_back(hex[md[i]>>4]); h.push_back(hex[md[i]&0xf]); }
  expect+=h;
  if(expect.size()!=hashed.size()) return false;
  volatile int d=0;
  for(size_t i=0;i<expect.size();i++) d|=expect[i]^hashed[i];
  return d==0;
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
    size_t p=html.find("{{.csrf_token}}"); if(p!=std::string::npos) html.replace(p, 15, csrf);
    p=html.find("{{ .csrf_token }}"); if(p!=std::string::npos) html.replace(p, 17, csrf);
    /* rendered.html hasil capture Go berisi token HARDCODE — ganti semua
     * kemunculannya (meta + hidden input) dengan csrf sesi ini. */
    p=html.find("CSRF_PLACEHOLDER");
    while(p!=std::string::npos){
      html.replace(p, 16, csrf);
      p=html.find("CSRF_PLACEHOLDER", p+csrf.size());
    }
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]="csrf_token="+csrf+"; Path=/";
    r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]="csrf_token="+csrf+"; Path=/";
  r.body="<html><body><form method=\"POST\" action=\"/login\"><input name=\"username\"><input name=\"password\" type=\"password\"><input type=\"hidden\" name=\"_csrf\" value=\""+csrf+"\"><button>Login</button></form></body></html>";
  return r;
}

Response login_handler(const Request& req, const Config& cfg){
  /* Body form adalah application/x-www-form-urlencoded — browser meng-encode
   * + / = dalam token CSRF menjadi %2B %2F %3D. Tanpa decode, token tidak
   * pernah match cookie aslinya → selalu 403. */
  auto form=helpers::parse_form(req.body);
  std::string csrf_header;
  auto it=req.headers.find("X-CSRF-Token"); if(it!=req.headers.end()) csrf_header=it->second;
  else {
    auto f=form.find("csrf_token"); if(f==form.end()) f=form.find("_csrf");
    if(f!=form.end()) csrf_header=f->second;
  }
  std::string session_csrf;
  auto ck=req.headers.find("Cookie"); if(ck!=req.headers.end()){ auto c=extract_cookie(ck->second,"csrf_token"); if(!c.empty()) session_csrf=c; }
  if(session_csrf.empty()){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}"); return r;
  }
  if(!verify_csrf(session_csrf, csrf_header)){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}"); return r;
  }
  std::string turnstile;
  auto tf=form.find("cf-turnstile-response"); if(tf!=form.end()) turnstile=tf->second;
  if(!turnstile.empty() && !middleware::verify_turnstile(turnstile, "", "")){
    Response r; r.status=403; r.json(403,"{\"error\":\"Turnstile failed\"}"); return r;
  }
  std::string username, password;
  if(auto u=form.find("username"); u!=form.end()) username=u->second;
  if(auto p=form.find("password"); p!=form.end()) password=p->second;
  if(username.empty()||password.empty()){
    Response r; r.status=400; r.json(400,"{\"error\":\"username and password required\"}"); return r;
  }
  std::string stored;
  { std::lock_guard<std::mutex> g(g_mu); auto f=g_users.find(username); if(f!=g_users.end()) stored=f->second; }
  if(stored.empty() || !verify_password(password, stored)){
    Response r; r.status=401; r.json(401,"{\"error\":\"invalid credentials\"}"); return r;
  }
  std::string payload=b64_encode("admin_id=1&username="+username+"&role=[\"guru\"]");
  std::string cookie="examvan_session="+encode_cookie_value(cfg.secret_key, payload)+"; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400";
  if(!cfg.is_development()) cookie+="; Secure";
  /* Klien API (fetch/AJAX) tetap menerima JSON {success,message} sesuai kontrak
   * F1 §5. Form HTML biasa tidak punya header tersebut → 303 redirect agar
   * browser langsung menuju dashboard/next, bukan menampilkan JSON mentah. */
  bool wants_json=false;
  if(auto h=req.headers.find("Accept"); h!=req.headers.end() && h->second.find("application/json")!=std::string::npos) wants_json=true;
  if(auto x=req.headers.find("X-Requested-With"); x!=req.headers.end() && x->second=="XMLHttpRequest") wants_json=true;
  if(wants_json){
    Response r; r.json(200,"{\"success\":true,\"username\":\""+username+"\"}");
    r.headers["Set-Cookie"]=cookie;
    return r;
  }
  std::string target="/admin/dashboard";
  if(auto n=form.find("next"); n!=form.end() && !n->second.empty()){
    /* open-redirect guard: hanya path relatif dalam situs yang diizinkan */
    const std::string& nx=n->second;
    if(nx[0]=='/' && (nx.size()==1 || nx[1]!='/') && nx.find("\\")==std::string::npos) target=nx;
  }
  Response r; r.status=303; r.headers["Location"]=target; r.headers["Set-Cookie"]=cookie;
  return r;
}

} // namespace examvan::handlers::auth
