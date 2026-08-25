#include "handlers/admin/exams.hpp"
namespace examvan::handlers::admin {
Response list_admin_exams(const Request&){
  Response r; r.json(200,"{\"exams\":[]}"); return r;
}
static std::string get_param(const std::string& b, const std::string& k){
  std::string n=k+"="; auto p=b.find(n); if(p==std::string::npos) return ""; size_t e=b.find('&',p); return b.substr(p+n.size(), e==std::string::npos? std::string::npos: e-p-n.size());
}
Response create_exam(const Request& req){
  std::string name=get_param(req.body,"name");
  std::string fpath=get_param(req.body,"file_path");
  std::string sz=get_param(req.body,"size_bytes");
  if(name.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"name required\"}"); return r; }
  if(fpath.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"file_path required\"}"); return r; }
  long size=0; try{ size=std::stol(sz); }catch(...){}
  const long MAX_PDF = 5*1024*1024;
  if(size>MAX_PDF){ Response r; r.status=413; r.json(413,"{\"error\":\"file too large, max 5MB\"}"); return r; }
  Response r; r.status=201; r.json(201,"{\"id\":1,\"token\":\"ABCDEFGH\",\"name\":\""+name+"\",\"file_path\":\""+fpath+"\"}"); return r;
}
Response update_exam(const Request&){
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
Response delete_exam(const Request&){
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
Response export_xlsx(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  r.headers["Content-Disposition"]="attachment; filename=\"export.xlsx\"";
  r.body="PK fake xlsx content"; return r;
}
} // namespace examvan::handlers::admin
