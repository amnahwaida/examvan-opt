#include "handlers/admin/pengawas.hpp"
#include "handlers/admin/template_helper.hpp"
namespace examvan::handlers::admin {
Response pengawas_page(const Request&){
  std::string html=render_admin_template("pengawas","2.7.2");
  if(!html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Pengawas</h1></body></html>"; return r;
}
Response pengawas_detail_page(const Request&){
  std::string html=render_admin_template("pengawas_detail","2.7.2");
  if(!html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Pengawas Detail</h1></body></html>"; return r;
}
Response pengawas_exams(const Request&){ Response r; r.json(200,"{\"success\":true,\"exams\":[],\"is_privileged\":true,\"page\":1,\"per_page\":10,\"total\":0,\"total_pages\":0,\"stats\":{\"total_exams\":0,\"active_exams\":0,\"total_students\":0,\"total_submitted\":0}}"); return r; }
Response pengawas_submissions(const Request&){ Response r; r.json(200,"{\"success\":true,\"exam_name\":\"\",\"exam_active_token\":\"\",\"submissions\":[],\"page\":1,\"per_page\":20,\"total\":0,\"total_pages\":0,\"stats\":{}}"); return r; }
Response pending_approvals(const Request&){ Response r; r.json(200,"{\"success\":true,\"data\":[],\"total\":0,\"page\":1,\"limit\":100}"); return r; }
Response set_approval(const Request&){ Response r; r.json(200,"{\"success\":true}"); return r; }
Response get_auto_approve(const Request&){ Response r; r.json(200,"{\"success\":true,\"enabled\":false}"); return r; }
Response set_auto_approve(const Request&){ Response r; r.json(200,"{\"success\":true,\"enabled\":false}"); return r; }
} // namespace examvan::handlers::admin
