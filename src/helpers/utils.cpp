#include "helpers/utils.hpp"
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace examvan::helpers {

std::string format_iso_utc(std::chrono::system_clock::time_point tp){
  std::time_t t = std::chrono::system_clock::to_time_t(tp);
  char buf[32];
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm,&t);
#else
  gmtime_r(&t, &tm);
#endif
  std::strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::optional<std::chrono::system_clock::time_point> parse_iso_utc(const std::string& s){
  std::tm tm{}; std::istringstream ss(s);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  if(ss.fail()) return std::nullopt;
  return std::chrono::system_clock::from_time_t(timegm(&tm));
}

std::string sanitize_student_input(const std::string& s){
  std::string out; out.reserve(s.size());
  bool last_space=false;
  for(char c: s){
    if(std::isspace((unsigned char)c)){ if(!last_space) out.push_back(' '); last_space=true; }
    else { out.push_back(c); last_space=false; }
  }
  size_t a=out.find_first_not_of(' '); if(a==std::string::npos) return "";
  size_t b=out.find_last_not_of(' '); return out.substr(a,b-a+1);
}

std::string generate_token(int len){
  static const char* chars="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  std::random_device rd; std::mt19937 g(rd());
  std::uniform_int_distribution<> d(0,31);
  std::string s; s.reserve(len); for(int i=0;i<len;i++) s.push_back(chars[d(g)]); return s;
}

std::string localize_utc(const std::string& utc_str, int offset_minutes){
  auto tp=parse_iso_utc(utc_str); if(!tp) return utc_str;
  auto local = *tp + std::chrono::minutes(offset_minutes);
  return format_iso_utc(local);
}

bool is_valid_exam_token(const std::string& t){
  if(t.size()<6||t.size()>32) return false;
  return std::all_of(t.begin(), t.end(), [](char c){ return std::isalnum((unsigned char)c); });
}

std::string round_to(double v, int decimals){
  std::ostringstream ss; ss<< std::fixed<< std::setprecision(decimals)<<v; return ss.str();
}

std::string url_decode(const std::string& s){
  std::string out; out.reserve(s.size());
  auto hex=[](char c)->int{
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
  };
  for(size_t i=0;i<s.size();++i){
    if(s[i]=='+'){ out+=' '; }
    else if(s[i]=='%' && i+2<s.size()){
      int h=hex(s[i+1]), l=hex(s[i+2]);
      if(h>=0&&l>=0){ out+=static_cast<char>((h<<4)|l); i+=2; }
      else out+=s[i];
    } else out+=s[i];
  }
  return out;
}

std::map<std::string,std::string> parse_form(const std::string& body){
  std::map<std::string,std::string> m;
  size_t start=0;
  while(start<=body.size()){
    size_t amp=body.find('&',start);
    std::string pair=body.substr(start, amp==std::string::npos? std::string::npos : amp-start);
    if(!pair.empty()){
      size_t eq=pair.find('=');
      std::string k = eq==std::string::npos? pair : pair.substr(0,eq);
      std::string v = eq==std::string::npos? "" : pair.substr(eq+1);
      m[url_decode(k)]=url_decode(v);
    }
    if(amp==std::string::npos) break;
    start=amp+1;
  }
  return m;
}

} // namespace examvan::helpers
