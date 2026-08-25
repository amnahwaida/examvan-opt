#include "middleware/cors.hpp"
#include <sstream>
namespace examvan::middleware {
bool is_origin_allowed(const std::string& origin, const std::string& csv){
  if(csv.empty()) return true;
  std::istringstream ss(csv); std::string t;
  while(std::getline(ss,t,',')){
    t.erase(0,t.find_first_not_of(" \t")); t.erase(t.find_last_not_of(" \t")+1);
    if(t==origin||t=="*") return true;
  }
  return false;
}
Response cors_wrap(const Request& req, const std::string& allowed, std::function<Response(const Request&)> next){
  auto it=req.headers.find("Origin");
  if(it!=req.headers.end() && !is_origin_allowed(it->second, allowed)){
    Response r; r.status=403; r.body="CORS forbidden"; return r;
  }
  auto res=next(req);
  if(it!=req.headers.end()) res.headers["Access-Control-Allow-Origin"]=it->second;
  return res;
}
} // namespace examvan::middleware
