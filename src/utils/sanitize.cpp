#include "utils/sanitize.hpp"
#include <algorithm>
#include <cctype>
#include <vector>

namespace examvan {

std::string sanitize_ws_field(const std::string& raw, size_t max_len) {
  std::string out;
  out.reserve(std::min(raw.size(), max_len));
  for (unsigned char c : raw) {
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'' || c == '`' || c == '=') continue;
    if (c < 0x20 || c == 0x7f) continue;
    out.push_back(static_cast<char>(c));
    if (out.size() >= max_len) break;
  }
  return out;
}

std::string sanitize_ws_mac(const std::string& raw) {
  std::string s;
  s.reserve(raw.size());
  for (unsigned char c : raw) {
    if (c < 0x20 || c == 0x7f) continue;
    s.push_back(static_cast<char>(c));
  }
  while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
  while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
  if (s.size() > 100) s.resize(100);
  return s;
}

std::string html_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size()*2);
  for(char c: s){
    if(c=='&') out+="&amp;";
    else if(c=='<') out+="&lt;";
    else if(c=='>') out+="&gt;";
    else if(c=='"') out+="&quot;";
    else if(c=='\'') out+="&#39;";
    else out.push_back(c);
  }
  return out;
}

std::string strip_sensitive_keys(const std::string& questions_json){
  std::string out=questions_json;
  for(const std::string& k: std::vector<std::string>{"key","answer"}){
    std::string needle=",\""+k+"\":";
    size_t p=0;
    while((p=out.find(needle,p))!=std::string::npos){
      size_t v=p+needle.size();
      size_t end=v;
      bool in_str=false, esc=false;
      int depth=0;
      for(; end<out.size(); ++end){
        char c=out[end];
        if(esc){ esc=false; continue; }
        if(c=='\\' && in_str){ esc=true; continue; }
        if(c=='"'){ in_str=!in_str; continue; }
        if(in_str) continue;
        if(c=='[' || c=='{') depth++;
        else if(c==']' || c=='}'){ if(depth==0) break; depth--; }
        else if(depth==0 && (c==',' || c=='}')) break;
      }
      if(end>v){
        out.erase(p, end-p);
      } else break;
    }
  }
  return out;
}

}
