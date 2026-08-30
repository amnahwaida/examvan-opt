#include "middleware/protobuf.hpp"
#include <algorithm>
#include <cctype>
namespace examvan::middleware {
static std::string to_lower_str(const std::string& s){
  std::string o=s; for(char &c:o) c=tolower((unsigned char)c); return o;
}
static std::string get_hdr(const Request& req, const std::string& name){
  for(auto &kv: req.headers){
    if(kv.first.size()!=name.size()) continue;
    bool eq=true; for(size_t i=0;i<name.size();++i) if(tolower((unsigned char)kv.first[i])!=tolower((unsigned char)name[i])){eq=false;break;}
    if(eq) return kv.second;
  }
  return "";
}
bool is_protobuf_content(const Request& req){
  std::string ct=get_hdr(req,"Content-Type");
  std::string low=to_lower_str(ct);
  return low.find("application/x-protobuf")!=std::string::npos || low.find("application/protobuf")!=std::string::npos || low.find("application/vnd.examvan")!=std::string::npos;
}
bool is_protobuf_accept(const Request& req){
  std::string ac=get_hdr(req,"Accept");
  std::string low=to_lower_str(ac);
  return low.find("application/x-protobuf")!=std::string::npos || low.find("application/protobuf")!=std::string::npos || low.find("application/vnd.examvan")!=std::string::npos;
}
std::optional<Response> require_protobuf(const Request& req, const Config& cfg){
  if(!cfg.protobuf_mandatory) return std::nullopt;
  // POST/PUT/PATCH with body must be protobuf
  if(req.method=="POST" || req.method=="PUT" || req.method=="PATCH"){
    if(!req.body.empty() && !is_protobuf_content(req)){
      Response r; r.status=415;
      r.headers["Content-Type"]="application/json";
      r.body="{\"success\":false,\"error_code\":\"PROTOBUF_REQUIRED\",\"message\":\"Gunakan application/x-protobuf\"}";
      return r;
    }
  }
  // If client explicitly asks for protobuf via Accept, we will honor; if it asks only json while mandatory, 406
  std::string ac=get_hdr(req,"Accept");
  if(!ac.empty()){
    std::string low=to_lower_str(ac);
    bool wants_json = low.find("application/json")!=std::string::npos;
    bool wants_pb = is_protobuf_accept(req);
    if(wants_json && !wants_pb){
      Response r; r.status=406;
      r.headers["Content-Type"]="application/json";
      r.body="{\"success\":false,\"error_code\":\"PROTOBUF_REQUIRED\",\"message\":\"Gunakan Accept: application/x-protobuf\"}";
      return r;
    }
  }
  return std::nullopt;
}
std::string protobuf_error_json(const std::string& code, const std::string& msg){
  return "{\"success\":false,\"error_code\":\""+code+"\",\"message\":\""+msg+"\"}";
}
}
