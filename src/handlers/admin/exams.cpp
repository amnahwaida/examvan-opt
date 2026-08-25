#include "handlers/admin/exams.hpp"
namespace examvan::handlers::admin {
Response list_admin_exams(const Request&){
  Response r; r.json(200,"{\"exams\":[]}"); return r;
}
Response create_exam(const Request&){
  Response r; r.status=201; r.json(201,"{\"id\":1,\"token\":\"ABCDEFGH\"}"); return r;
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
