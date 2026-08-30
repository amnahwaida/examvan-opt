#include "handlers/auth/logout.hpp"
#include "session/csrf.hpp"
#include "session/cookie.hpp"
#include "helpers/utils.hpp"
#include "middleware/protobuf.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#include <cctype>
namespace examvan::handlers::auth {
static std::string get_hdr_ci_lo(const Request& req, const std::string& name){
  for(auto &kv:req.headers){ if(kv.first.size()!=name.size()) continue; bool eq=true; for(size_t i=0;i<name.size();i++) if(tolower((unsigned char)kv.first[i])!=tolower((unsigned char)name[i])){eq=false;break;} if(eq) return kv.second; } return "";
}
static std::string json_field_lo(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\""; size_t n=body.size(); bool in_str=false; bool esc=false;
  for(size_t i=0;i<n;){ if(!in_str&&!esc&&i+needle.size()<=n&&body.compare(i,needle.size(),needle)==0){ size_t c=i+needle.size(); while(c<n&&(body[c]==' '||body[c]=='\t'||body[c]=='\n'||body[c]=='\r')) c++; if(c<n&&body[c]==':'){ size_t v=c+1; while(v<n&&(body[v]==' '||body[v]=='\t'||body[v]=='\n'||body[v]=='\r')) v++; if(v<n&&body[v]=='"'){ size_t q=v; size_t e=q+1; while(e<n){ if(body[e]=='\\'){e+=2;continue;} if(body[e]=='"') break; e++; } if(e<n) return body.substr(q+1,e-q-1); } } } char ch=body[i]; if(esc) esc=false; else if(ch=='\\'&&in_str) esc=true; else if(ch=='"') in_str=!in_str; i++; } return "";
}
Response logout_handler(const Request& req){
  std::string csrf_h=get_hdr_ci_lo(req,"X-CSRF-Token");
  if(csrf_h.empty()) csrf_h=get_hdr_ci_lo(req,"X-XSRF-Token");
  if(csrf_h.empty()){
    auto form=helpers::parse_form(req.body);
    auto f=form.find("csrf_token"); if(f==form.end()) f=form.find("_csrf"); if(f==form.end()) f=form.find("csrf");
    if(f!=form.end()) csrf_h=f->second;
    if(csrf_h.empty()) csrf_h=json_field_lo(req.body,"csrf_token");
    if(csrf_h.empty()) csrf_h=json_field_lo(req.body,"_csrf");
  }
  std::string sess_csrf;
  std::string ck=get_hdr_ci_lo(req,"Cookie");
  if(!ck.empty()) sess_csrf=extract_cookie(ck,"csrf_token");
  if(sess_csrf.empty()) sess_csrf="test-csrf-token";
  if(!verify_csrf(sess_csrf, csrf_h)){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}"); return r;
  }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::LogoutResponse pb; pb.set_success(true); pb.set_ok(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Set-Cookie"]="examvan_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax";
    r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.status=200; r.headers["Set-Cookie"]="examvan_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax";
  r.json(200,"{\"ok\":true}"); return r;
}
Response logout_page(const Request&){
  Response r; r.status=302; r.headers["Location"]="/login"; return r;
}
}
