#include "helpers/utils.hpp"
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace examvan::helpers {

std::string format_iso_utc(std::chrono::system_clock::time_point tp){
  std::time_t t = std::chrono::system_clock::to_time_t(tp);
  char buf[32]; std::strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
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

} // namespace examvan::helpers
