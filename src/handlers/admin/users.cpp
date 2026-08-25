#include "handlers/admin/users.hpp"
#include "models/user.hpp"
#include <string>
namespace examvan::handlers::admin {
Response list_users(const Request&){
  Response r; r.json(200,"{\"users\":[],\"total\":0}"); return r;
}
Response create_user(const Request& req){
  if(req.body.find("username")==std::string::npos){ Response r; r.status=400; r.json(400,"{\"error\":\"username required\"}"); return r; }
  Response r; r.json(200,"{\"ok\":true,\"id\":1}"); return r;
}
Response edit_user(const Request&){
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
Response delete_user(const Request&){
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
Response instansi_update(const Request&){
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
} // namespace examvan::handlers::admin
