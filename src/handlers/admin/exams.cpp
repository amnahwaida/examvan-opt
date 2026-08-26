#include "handlers/admin/exams.hpp"
#include "helpers/utils.hpp"
namespace examvan::handlers::admin {
Response list_admin_exams(const Request&){
  Response r; r.json(200,"{\"exams\":[]}"); return r;
}
static std::string get_param(const std::map<std::string,std::string>& form, const std::string& k){
  auto it=form.find(k); return it!=form.end()? it->second : "";
}
Response create_exam(const Request& req){
  auto form=helpers::parse_form(req.body);
  std::string name=get_param(form,"name");
  std::string fpath=get_param(form,"file_path");
  std::string sz=get_param(form,"size_bytes");
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
