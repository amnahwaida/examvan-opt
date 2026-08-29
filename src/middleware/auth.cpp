#include "middleware/auth.hpp"
#include "config/config.hpp"
namespace examvan::middleware {
bool is_authenticated(const Request& req, const std::string& secret, SessionData* out){
  auto it=req.headers.find("Cookie");
  if(it==req.headers.end()) return false;
  std::string prev;
  if(auto* c=getenv("EXAMVAN_SECRET_PREV")) prev=c;
  auto sess=prev.empty()? verify_session_cookie(secret, it->second) : verify_session_cookie_dual(secret, prev, it->second);
  if(!sess) return false;
  if(out) *out=*sess;
  return sess->admin_id!=0;
}
Response require_auth(const Request& req, const std::string& secret, std::function<Response(const Request&, const SessionData&)> next){
  SessionData s;
  if(!is_authenticated(req, secret, &s)){
    Response r; r.status=302; r.headers["Location"]="/login"; r.body="redirect"; return r;
  }
  return next(req,s);
}
bool require_role(const SessionData& s, const std::string& role){
  return s.role.find(role)!=std::string::npos || s.is_super_admin;
}
} // namespace examvan::middleware
