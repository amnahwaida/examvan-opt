#include "middleware/cors.hpp"
#include <sstream>
namespace examvan::middleware {
bool is_origin_allowed(const std::string& origin, const std::string& csv){
  if(csv.empty()) return false;
  if(origin.empty()) return false;
  if(csv=="*") return false;
  std::istringstream ss(csv); std::string t;
  while(std::getline(ss,t,',')){
    size_t s=t.find_first_not_of(" \t"); if(s==std::string::npos) continue;
    size_t e=t.find_last_not_of(" \t"); t=t.substr(s,e-s+1);
    if(t=="*") continue;
    if(t==origin) return true;
  }
  return false;
}
Response cors_wrap(const Request& req, const std::string& allowed, std::function<Response(const Request&)> next){
  auto it=req.headers.find("Origin");
  if(it!=req.headers.end() && !is_origin_allowed(it->second, allowed)){
    Response r; r.status=403; r.body="CORS forbidden"; r.headers["Vary"]="Origin"; return r;
  }
  auto res=next(req);
  if(it!=req.headers.end()){
    res.headers["Access-Control-Allow-Origin"]=it->second;
    res.headers["Vary"]="Origin";
  }
  return res;
}
} // namespace examvan::middleware
