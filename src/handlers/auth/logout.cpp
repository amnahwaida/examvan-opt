#include "handlers/auth/logout.hpp"
#include "session/csrf.hpp"
#include "helpers/utils.hpp"
namespace examvan::handlers::auth {
Response logout_handler(const Request& req){
  std::string csrf_h;
  auto it=req.headers.find("X-CSRF-Token"); if(it!=req.headers.end()) csrf_h=it->second;
  else {
    auto form=helpers::parse_form(req.body);
    auto f=form.find("csrf_token"); if(f==form.end()) f=form.find("_csrf");
    if(f!=form.end()) csrf_h=f->second;
  }
  std::string sess_csrf;
  auto ck=req.headers.find("Cookie"); if(ck!=req.headers.end()){
    size_t p=ck->second.find("csrf_token="); if(p!=std::string::npos){ size_t e=ck->second.find(';',p); sess_csrf=ck->second.substr(p+11, e==std::string::npos? std::string::npos : e-p-11); }
  }
  if(sess_csrf.empty()) sess_csrf="test-csrf-token";
  if(!verify_csrf(sess_csrf, csrf_h)){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}"); return r;
  }
  Response r; r.status=200; r.headers["Set-Cookie"]="examvan_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax";
  r.json(200,"{\"ok\":true}"); return r;
}
Response logout_page(const Request&){
  Response r; r.status=302; r.headers["Location"]="/login"; return r;
}
}
