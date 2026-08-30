#include "handlers/admin/users.hpp"
#include "models/user.hpp"
#include "helpers/utils.hpp"
#include "session/cookie.hpp"
#include "config/config.hpp"
#include <string>
namespace examvan::handlers::admin {

static std::string get_param(const std::map<std::string,std::string>& form, const std::string& key){
  auto it=form.find(key); return it!=form.end()? it->second : "";
}

Response list_users(const Request&){
  Response r; r.json(200,"{\"users\":[],\"total\":0}"); return r;
}

Response create_user(const Request& req){
  auto form=helpers::parse_form(req.body);
  std::string username=get_param(form,"username");
  std::string password=get_param(form,"password");
  std::string role=get_param(form,"role"); if(role.empty()) role="guru";
  if(username.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"username required\"}"); return r; }
  if(!models::is_valid_username(username)){ Response r; r.status=400; r.json(400,"{\"error\":\"username 3-32 lowercase, dot, underscore, hyphen\"}"); return r; }
  if(password.size()<8){ Response r; r.status=400; r.json(400,"{\"error\":\"password minimal 8 karakter\"}"); return r; }
  if(role=="operator"){
    std::string cookie;
    auto itc=req.headers.find("Cookie");
    if(itc!=req.headers.end()) cookie=itc->second;
    auto cfg = Config::load();
    std::string cur=cfg.secret_key;
    std::string prev=cfg.secret_prev;
    auto sess=prev.empty()?verify_session_cookie(cur, cookie):verify_session_cookie_dual(cur, prev, cookie);
    bool is_super=false;
    if(sess){
      is_super=sess->is_super_admin || sess->role=="superadmin";
    }
    if(!is_super){ Response r; r.status=403; r.json(403,"{\"error\":\"hanya superadmin bisa buat operator\"}"); return r; }
  }
  if(role!="guru" && role!="pengawas" && role!="operator"){ Response r; r.status=400; r.json(400,"{\"error\":\"role tidak valid\"}"); return r; }
  Response r; r.status=201; r.json(201,"{\"ok\":true,\"id\":1,\"username\":\""+username+"\",\"role\":\""+role+"\"}"); return r;
}
Response edit_user(const Request&){
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
Response delete_user(const Request&){
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
Response instansi_update(const Request& req){
  auto form=helpers::parse_form(req.body);
  std::string name=get_param(form,"instansi");
  if(name.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"instansi required\"}"); return r; }
  Response r; r.json(200,"{\"ok\":true,\"instansi\":\""+name+"\"}"); return r;
}
} // namespace examvan::handlers::admin
