#include "handlers/auth/auth_helpers.hpp"
#include "session/csrf.hpp"
#include "session/cookie.hpp"
#include "helpers/utils.hpp"
#include <cctype>

namespace examvan::handlers::auth {

std::string get_hdr_ci(const Request& req, const std::string& name){
  for(const auto& kv: req.headers){
    if(kv.first.size()!=name.size()) continue;
    bool eq=true;
    for(size_t i=0;i<name.size();i++)
      if(tolower((unsigned char)kv.first[i])!=tolower((unsigned char)name[i])){ eq=false; break; }
    if(eq) return kv.second;
  }
  return "";
}

std::string client_ip(const Request& req){
  std::string ip=get_hdr_ci(req,"X-Real-IP");
  if(ip.empty()){
    std::string fwd=get_hdr_ci(req,"X-Forwarded-For");
    if(!fwd.empty()){
      size_t c=fwd.find(',');
      ip=c==std::string::npos? fwd : fwd.substr(0,c);
    }
  }
  // Trim spasi.
  size_t a=ip.find_first_not_of(" \t\r\n");
  size_t b=ip.find_last_not_of(" \t\r\n");
  if(a==std::string::npos) return "global";
  return ip.substr(a,b-a+1);
}

static std::string json_field(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t n=body.size(); bool in_str=false; bool esc=false;
  for(size_t i=0;i<n;){
    if(!in_str && !esc && i+needle.size()<=n && body.compare(i,needle.size(),needle)==0){
      size_t c=i+needle.size(); while(c<n && (body[c]==' '||body[c]=='\t'||body[c]=='\n'||body[c]=='\r')) c++;
      if(c<n && body[c]==':'){ size_t v=c+1; while(v<n && (body[v]==' '||body[v]=='\t'||body[v]=='\n'||body[v]=='\r')) v++;
        if(v<n && body[v]=='"'){ size_t q=v; size_t e=q+1; while(e<n){ if(body[e]=='\\'){e+=2;continue;} if(body[e]=='"') break; e++; } if(e<n) return body.substr(q+1,e-q-1); }
        else if(v<n){ size_t e=v; while(e<n && body[e]!=',' && body[e]!='}' && body[e]!='"') e++; std::string val=body.substr(v,e-v); size_t a=val.find_first_not_of(" \t\r\n"); size_t b=val.find_last_not_of(" \t\r\n"); if(a!=std::string::npos) val=val.substr(a,b-a+1); return val; }
      }
    }
    char ch=body[i]; if(esc) esc=false; else if(ch=='\\' && in_str) esc=true; else if(ch=='"') in_str=!in_str; i++;
  }
  return "";
}

std::string request_csrf_token(const Request& req, const std::map<std::string,std::string>& form){
  std::string tok=get_hdr_ci(req,"X-CSRF-Token");
  if(tok.empty()) tok=get_hdr_ci(req,"X-XSRF-Token");
  if(tok.empty()){
    // M5: pilih jalur ekstraksi SESUAI Content-Type — body JSON tidak boleh
    // dibaca sebagai form (dan sebaliknya). Tanpa ini body ambigu bisa
    // diekstrak lewat jalur yang dikontrol halaman attacker, melemahkan
    // skema double-submit.
    std::string ct=get_hdr_ci(req,"Content-Type");
    bool is_json = ct.find("application/json")!=std::string::npos;
    bool is_form = ct.find("application/x-www-form-urlencoded")!=std::string::npos ||
                   ct.find("multipart/form-data")!=std::string::npos;
    if(is_form || (!is_json && ct.empty())){
      auto f=form.find("csrf_token"); if(f==form.end()) f=form.find("_csrf"); if(f==form.end()) f=form.find("csrf");
      if(f!=form.end()) tok=f->second;
    }
    if(tok.empty() && is_json){
      tok=json_field(req.body,"csrf_token");
      if(tok.empty()) tok=json_field(req.body,"_csrf");
      if(tok.empty()) tok=json_field(req.body,"csrf");
      if(tok.empty()) tok=json_field(req.body,"x-csrf-token");
    }
    // Content-Type lain (mis. text/plain) tanpa header CSRF → tidak ada token.
  }
  return tok;
}

bool csrf_ok(const Request& req, const std::map<std::string,std::string>& form){
  std::string cookie_hdr=get_hdr_ci(req,"Cookie");
  if(cookie_hdr.empty()) return false;
  std::string session_csrf=extract_cookie(cookie_hdr,"csrf_token");
  if(session_csrf.empty()) return false;
  std::string tok=request_csrf_token(req, form);
  if(tok.empty()) return false;
  return verify_csrf(session_csrf, tok);
}

bool wants_json(const Request& req){
  if(get_hdr_ci(req,"Accept").find("application/json")!=std::string::npos) return true;
  if(get_hdr_ci(req,"X-Requested-With")=="XMLHttpRequest") return true;
  std::string ct=get_hdr_ci(req,"Content-Type");
  return ct.find("application/json")!=std::string::npos;
}

Response success_response(bool json, const std::string& location, const std::string& json_body){
  if(json){ Response r; r.json(200, json_body); return r; }
  Response r; r.status=303; r.headers["Location"]=location; return r;
}

std::string url_encode(const std::string& s){
  static const char* hex="0123456789ABCDEF";
  std::string o;
  for(unsigned char c: s){
    if(isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~') o.push_back((char)c);
    else { o.push_back('%'); o.push_back(hex[c>>4]); o.push_back(hex[c&15]); }
  }
  return o;
}

} // namespace examvan::handlers::auth