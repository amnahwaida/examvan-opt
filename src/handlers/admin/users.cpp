#include "handlers/admin/users.hpp"
#include "models/user.hpp"
#include <string>
#include <regex>
namespace examvan::handlers::admin {
Response list_users(const Request&){
  Response r; r.json(200,"{\"users\":[],\"total\":0}"); return r;
}
static std::string get_param(const std::string& body, const std::string& key){
  std::string needle=key+"=";
  auto p=body.find(needle); if(p==std::string::npos) return "";
  size_t e=body.find('&',p); return body.substr(p+needle.size(), e==std::string::npos? std::string::npos: e-p-needle.size());
}
Response create_user(const Request& req){
  std::string username=get_param(req.body,"username");
  std::string password=get_param(req.body,"password");
  std::string role=get_param(req.body,"role"); if(role.empty()) role="guru";
  if(username.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"username required\"}"); return r; }
  if(!models::is_valid_username(username)){ Response r; r.status=400; r.json(400,"{\"error\":\"username 3-32 lowercase, dot, underscore, hyphen\"}"); return r; }
  if(password.size()<8){ Response r; r.status=400; r.json(400,"{\"error\":\"password minimal 8 karakter\"}"); return r; }
  if(role=="operator"){
    auto it=req.headers.find("X-Role");
    std::string caller=it!=req.headers.end()?it->second:"guru";
    if(caller!="superadmin"){ Response r; r.status=403; r.json(403,"{\"error\":\"hanya superadmin bisa buat operator\"}"); return r; }
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
  std::string name=get_param(req.body,"instansi");
  if(name.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"instansi required\"}"); return r; }
  Response r; r.json(200,"{\"ok\":true,\"instansi\":\""+name+"\"}"); return r;
}
} // namespace examvan::handlers::admin
