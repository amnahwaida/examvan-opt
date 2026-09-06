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
    size_t search=0;
    while(true){
      const std::string needle="\""+k+"\":";
      size_t p=out.find(needle,search);
      if(p==std::string::npos) break;
      // Only accept an object member: the byte before the name must be '{' or
      // ',' (allowing whitespace). This also handles the first member, which
      // the old comma-prefixed search missed.
      size_t before=p;
      while(before>0 && (out[before-1]==' '||out[before-1]=='\t'||out[before-1]=='\r'||out[before-1]=='\n')) --before;
      if(before==0 || (out[before-1]!='{' && out[before-1]!=',')){
        search=p+needle.size();
        continue;
      }
      size_t v=p+needle.size();
      while(v<out.size() && (out[v]==' '||out[v]=='\t'||out[v]=='\r'||out[v]=='\n')) ++v;
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
        else if(c==']' || c=='}'){ if(depth==0) break; --depth; }
        else if(depth==0 && (c==',' || c=='}')) break;
      }
      if(end<=v) break;
      // Remove preceding comma for non-first members. For first members,
      // preserve the opening brace and remove the following comma as well.
      size_t erase_start=p;
      size_t erase_len=end-p;
      if(out[before-1]==',') { erase_start=before-1; ++erase_len; }
      else if(end<out.size() && out[end]==',') ++erase_len;
      out.erase(erase_start,erase_len);
      search=erase_start;
    }
  }
  return out;
}

}
