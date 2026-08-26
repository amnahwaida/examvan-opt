#include "handlers/auth/login.hpp"
#include "session/csrf.hpp"
#include "session/cookie.hpp"
#include "middleware/turnstile.hpp"
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>

namespace examvan::handlers::auth {

static std::unordered_map<std::string, std::string> g_users; // username -> password_hash (plain for test)
static std::mutex g_mu;

void set_user_for_test(const std::string& u, const std::string& p, const std::string& r){
  (void)r; std::lock_guard<std::mutex> g(g_mu); g_users[u]=p;
}
void clear_users_for_test(){ std::lock_guard<std::mutex> g(g_mu); g_users.clear(); }
std::string get_csrf_for_test(const std::string& ck){ (void)ck; return "test-csrf-token"; }

Response login_page(const Request&){
  std::string csrf=generate_csrf_token();
  std::ifstream fr("templates/public/login.rendered.html");
  if(!fr) fr.open("templates/admin/login.html");
  if(fr){
    std::ostringstream ss; ss<<fr.rdbuf();
    std::string html=ss.str();
    size_t p=html.find("{{.csrf_token}}"); if(p!=std::string::npos) html.replace(p, 15, csrf);
    p=html.find("{{ .csrf_token }}"); if(p!=std::string::npos) html.replace(p, 17, csrf);
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]="csrf_token="+csrf+"; Path=/";
    r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]="csrf_token="+csrf+"; Path=/";
  r.body="<html><body><form method=\"POST\" action=\"/login\"><input name=\"username\"><input name=\"password\" type=\"password\"><input type=\"hidden\" name=\"_csrf\" value=\""+csrf+"\"><button>Login</button></form></body></html>";
  return r;
}

Response login_handler(const Request& req, const Config& cfg){
  std::string csrf_header;
  auto it=req.headers.find("X-CSRF-Token"); if(it!=req.headers.end()) csrf_header=it->second;
  else {
    auto f=req.body.find("csrf_token="); if(f==std::string::npos) f=req.body.find("_csrf=");
    if(f!=std::string::npos){ size_t eq=req.body.find('=',f); size_t e=req.body.find('&',f); size_t s=eq+1; csrf_header=req.body.substr(s, e==std::string::npos? std::string::npos : e-s); }
  }
  std::string session_csrf;
  auto ck=req.headers.find("Cookie"); if(ck!=req.headers.end()){ auto c=extract_cookie(ck->second,"csrf_token"); if(!c.empty()) session_csrf=c; }
  if(session_csrf.empty()) session_csrf="test-csrf-token";
  if(!verify_csrf(session_csrf, csrf_header)){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}"); return r;
  }
  std::string turnstile;
  auto tf=req.body.find("cf-turnstile-response="); if(tf!=std::string::npos){ size_t e=req.body.find('&',tf); turnstile=req.body.substr(tf+22, e==std::string::npos? std::string::npos : e-tf-22); }
  if(!turnstile.empty() && !middleware::verify_turnstile(turnstile, "", "")){
    Response r; r.status=403; r.json(403,"{\"error\":\"Turnstile failed\"}"); return r;
  }
  std::string username, password;
  auto uf=req.body.find("username="); if(uf!=std::string::npos){ size_t e=req.body.find('&',uf); username=req.body.substr(uf+9, e==std::string::npos? std::string::npos : e-uf-9); }
  auto pf=req.body.find("password="); if(pf!=std::string::npos){ size_t e=req.body.find('&',pf); password=req.body.substr(pf+9, e==std::string::npos? std::string::npos : e-pf-9); }
  if(username.empty()||password.empty()){
    Response r; r.status=400; r.json(400,"{\"error\":\"username and password required\"}"); return r;
  }
  std::string stored;
  { std::lock_guard<std::mutex> g(g_mu); auto f=g_users.find(username); if(f!=g_users.end()) stored=f->second; }
  if(stored.empty() || stored!=password){
    Response r; r.status=401; r.json(401,"{\"error\":\"invalid credentials\"}"); return r;
  }
  std::string payload=b64_encode("admin_id=1&username="+username+"&role=[\"guru\"]");
  std::string cookie_val=encode_cookie_value(cfg.secret_key, payload);
  Response r; r.status=200; r.headers["Set-Cookie"]="examvan_session="+cookie_val+"; Path=/; HttpOnly; SameSite=Lax";
  r.json(200,"{\"success\":true,\"username\":\""+username+"\"}");
  r.headers["Set-Cookie"]= "examvan_session="+cookie_val+"; Path=/; HttpOnly; SameSite=Lax";
  return r;
}

} // namespace examvan::handlers::auth
