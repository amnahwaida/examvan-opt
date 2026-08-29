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

static std::string url_decode_simple(const std::string& s){
  std::string o; o.reserve(s.size());
  for(size_t i=0;i<s.size();){
    if(s[i]=='%' && i+2<s.size()){
      auto hx=[](char c)->int{ if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; };
      int h=hx(s[i+1]), l=hx(s[i+2]);
      if(h>=0&&l>=0){ o.push_back(char((h<<4)|l)); i+=3; continue; }
    }
    if(s[i]=='+') o.push_back(' '); else o.push_back(s[i]);
    i++;
  }
  return o;
}
std::string pg_conninfo_from_url(const std::string& u){
  size_t proto = u.find("://");
  if(proto==std::string::npos) return "";
  std::string scheme=u.substr(0,proto);
  if(scheme!="postgresql" && scheme!="postgres") return "";
  std::string rest=u.substr(proto+3);
  size_t at = rest.find('@');
  std::string userinfo, hostpart;
  if(at!=std::string::npos){ userinfo=rest.substr(0,at); hostpart=rest.substr(at+1); }
  else hostpart=rest;
  std::string user, pass;
  if(!userinfo.empty()){
    size_t colon=userinfo.find(':');
    if(colon!=std::string::npos){ user=url_decode_simple(userinfo.substr(0,colon)); pass=url_decode_simple(userinfo.substr(colon+1)); }
    else user=url_decode_simple(userinfo);
  }
  size_t slash=hostpart.find('/');
  size_t q=hostpart.find('?');
  std::string hostport = hostpart.substr(0, std::min(slash,q));
  std::string dbname;
  if(slash!=std::string::npos){
    size_t db_end = q==std::string::npos? hostpart.size():q;
    dbname=hostpart.substr(slash+1, db_end-slash-1);
    dbname=url_decode_simple(dbname);
  }
  std::string host=hostport;
  std::string port;
  size_t colon=hostport.rfind(':');
  if(colon!=std::string::npos && hostport.find(']')==std::string::npos){
    host=hostport.substr(0,colon);
    port=hostport.substr(colon+1);
  }
  if(host.empty()) host="db";
  if(dbname.empty()) dbname="examvan";
  std::string ci="host="+host;
  if(!port.empty()) ci+=" port="+port;
  if(!user.empty()) ci+=" user="+user;
  if(!pass.empty()) ci+=" password="+pass;
  ci+=" dbname="+dbname;
  size_t qpos=u.find('?');
  if(qpos!=std::string::npos){
    std::string qs=u.substr(qpos+1);
    if(qs.find("sslmode=")!=std::string::npos) ci+=" sslmode=require";
  }
  return ci;
}

}  // namespace examvan
