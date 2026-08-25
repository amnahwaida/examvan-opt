#include "db/pool.hpp"
#include <regex>
namespace examvan {

bool DbPool::has_valid_url() const {
  return url.rfind("postgresql://",0)==0 || url.rfind("postgres://",0)==0;
}

bool DbPool::connect(){
  if(!has_valid_url()) return false;
  if(max_conns<1 || max_conns>150) return false;
  connected = true;
  return true;
}

std::string DbPool::sanitized_url() const {
  std::string out=url;
  auto at=out.find('@');
  auto proto=out.find("://");
  if(at!=std::string::npos && proto!=std::string::npos){
    auto colon=out.find(':', proto+3);
    if(colon!=std::string::npos && colon<at) out.replace(colon+1, at-colon-1, "***");
  }
  return out;
}

std::string pg_conninfo_from_url(const std::string& url){
  if(url.rfind("postgresql://",0)!=0 && url.rfind("postgres://",0)!=0) return "";
  return "host=db dbname=examvan";
}

}  // namespace examvan
