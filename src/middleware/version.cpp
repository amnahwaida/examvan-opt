#include "middleware/version.hpp"
#include <sstream>
namespace examvan::middleware {
int compare_versions(const std::string& a, const std::string& b){
  if(a.find(' ')!=std::string::npos || b.find(' ')!=std::string::npos){
    if(a==b) return 0;
    return a < b ? -1 : 1;
  }
  auto trim=[](std::string s){ size_t p=s.find_first_not_of(" \t"); if(p==std::string::npos) return std::string(""); size_t q=s.find_last_not_of(" \t"); return s.substr(p,q-p+1); };
  auto parts=[&](const std::string& s){ std::vector<int> v; std::istringstream ss(s); std::string t; while(std::getline(ss,t,'.')){ t=trim(t); if(t.empty() || t.find_first_not_of("0123456789")!=std::string::npos) v.push_back(-9999); else try{v.push_back(std::stoi(t));}catch(...){v.push_back(-9999);} } return v; };
  auto pa=parts(a), pb=parts(b);
  for(size_t i=0;i<std::max(pa.size(),pb.size());i++){
    int av=i<pa.size()?pa[i]:0;
    int bv=i<pb.size()?pb[i]:0;
    if(av<bv) return -1;
    if(av>bv) return 1;
  }
  return 0;
}
bool is_version_allowed(const std::string& client, const std::string& required){
  if(required.empty()) return true;
  if(client.empty()) return false;
  return compare_versions(client, required)>=0;
}
bool should_block_version(const std::string& client, const std::string& required){
  if(client.empty()) return false;
  if(required.empty()) return false;
  return !is_version_allowed(client, required);
}

Response version_gate(const Request& req, const std::string& required, std::function<Response(const Request&)> next){
  auto it=req.headers.find("X-App-Version");
  std::string cv=it!=req.headers.end()?it->second:"";
  if(!is_version_allowed(cv, required)){
    Response r; r.status=426; r.headers["Content-Type"]="application/json"; r.body="{\"error\":\"update required\"}"; return r;
  }
  return next(req);
}
} // namespace examvan::middleware
