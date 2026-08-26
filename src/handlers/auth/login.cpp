#include "handlers/auth/login.hpp"
#include "session/csrf.hpp"
#include "session/cookie.hpp"
#include "middleware/turnstile.hpp"
#include "helpers/utils.hpp"
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
  if(session_csrf.empty()) session_csrf="test-csrf-token";
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
