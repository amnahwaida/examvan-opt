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
  /* P33-G4: query-string diparse UTUH menjadi pasangan key=value (dulu:
   * kehadiran `sslmode=` apa pun dipaksa jadi `sslmode=require` —
   * verify-full operator diturunkan senyap (MITM tak terdeteksi), disable
   * untuk PG lokal no-TLS membuat koneksi gagal, connect_timeout /
   * application_name / sslrootcert dibuang). Kini: parameter klien
   * diteruskan apa adanya; default `require` hanya bila sslmode ABSEN
   * (dengan ATAU tanpa query-string — URL polos pun terlindungi TLS). */
  bool have_sslmode=false;
  if(qpos!=std::string::npos){
    std::string qs=u.substr(qpos+1);
    size_t pos=0;
    while(pos<qs.size()){
      size_t amp=qs.find('&',pos);
      if(amp==std::string::npos) amp=qs.size();
      std::string pair=qs.substr(pos,amp-pos);
      pos=amp+1;
      if(pair.empty()) continue;
      size_t eq=pair.find('=');
      if(eq==std::string::npos) continue;
      std::string key=pair.substr(0,eq), val=pair.substr(eq+1);
      if(key.empty()) continue;
      // URL-decode sederhana nilai (%XX dan '+' → spasi).
      std::string dec; dec.reserve(val.size());
      for(size_t i=0;i<val.size();++i){
        if(val[i]=='+' ) dec+=' ';
        else if(val[i]=='%' && i+2<val.size()){
          auto hex=[](char c)->int{ if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; };
          int h=hex(val[i+1]), l=hex(val[i+2]);
          if(h>=0&&l>=0){ dec+=static_cast<char>(h*16+l); i+=2; } else dec+=val[i];
        } else dec+=val[i];
      }
      if(key=="sslmode") have_sslmode=true;
      ci+=" "+key+"="+dec;
    }
  }
  if(!have_sslmode) ci+=" sslmode=require";
  return ci;
}

std::string conninfo_from_url_or_raw(const std::string& url){
  std::string ci=pg_conninfo_from_url(url);
  return ci.empty()? url : ci;
}

}  // namespace examvan
