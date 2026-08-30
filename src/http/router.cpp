#include "http/router.hpp"
#include "helpers/utils.hpp"
#include <sstream>

namespace examvan {

std::map<std::string,std::string> Request::cookies() const {
  std::map<std::string,std::string> m;
  auto it=headers.find("Cookie"); if(it==headers.end()) it=headers.find("cookie");
  if(it==headers.end()) return m;
  std::istringstream ss(it->second);
  std::string tok;
  while(std::getline(ss,tok,';')){
    auto eq=tok.find('=');
    if(eq==std::string::npos) continue;
    std::string k=tok.substr(0,eq), v=tok.substr(eq+1);
    k.erase(0,k.find_first_not_of(" \t")); k.erase(k.find_last_not_of(" \t")+1);
    v.erase(0,v.find_first_not_of(" \t")); v.erase(v.find_last_not_of(" \t")+1);
    m[k]=v;
  }
  return m;
}

void Response::json(int code, const std::string& j){ status=code; headers["Content-Type"]="application/json"; body=j; }
void Response::text(int code, const std::string& t){ status=code; headers["Content-Type"]="text/plain"; body=t; }

void Router::add(const std::string& method, const std::string& path, Handler h){
  routes_.push_back({method,path,std::move(h)});
}

bool Router::match(const std::string& pat, const std::string& path, std::map<std::string,std::string>& out){
  std::string dec_path = helpers::url_decode(path);
  if(pat==dec_path) return true;
  std::vector<std::string> pp, ap;
  auto split=[&](const std::string& s, std::vector<std::string>& v){
    std::istringstream ss(s); std::string t; while(std::getline(ss,t,'/')) if(!t.empty()) v.push_back(t);
  };
  split(pat,pp); split(dec_path,ap);
  if(pp.size()!=ap.size()) return false;
  for(size_t i=0;i<pp.size();++i){
    if(!pp[i].empty() && pp[i][0]==':') out[pp[i].substr(1)]=helpers::url_decode(ap[i]);
    else if(pp[i]!=ap[i]) return false;
  }
  return true;
}

Response Router::dispatch(const Request& req) const{
  for(auto& r: routes_){
    if(r.method!=req.method) continue;
    std::map<std::string,std::string> params;
    if(match(r.path, req.path, params)){
      Request nr=req; nr.params=params;
      return r.handler(nr);
    }
  }
  Response res; res.status=404; res.body="{\"error\":\"not found\"}"; res.headers["Content-Type"]="application/json"; return res;
}

std::vector<std::string> Router::routes() const{
  std::vector<std::string> v; for(auto& r: routes_) v.push_back(r.method+" "+r.path); return v;
}

}  // namespace examvan
