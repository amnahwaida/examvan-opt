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
Response pengawas_exams(const Request&){ Response r; r.json(200,"{\"exams\":[]}"); return r; }
Response pengawas_submissions(const Request&){ Response r; r.json(200,"{\"submissions\":[]}"); return r; }
Response pending_approvals(const Request&){ Response r; r.json(200,"{\"approvals\":[]}"); return r; }
Response set_approval(const Request&){ Response r; r.json(200,"{\"ok\":true}"); return r; }
Response get_auto_approve(const Request&){ Response r; r.json(200,"{\"auto_approve\":false}"); return r; }
Response set_auto_approve(const Request&){ Response r; r.json(200,"{\"ok\":true}"); return r; }
} // namespace examvan::handlers::admin
