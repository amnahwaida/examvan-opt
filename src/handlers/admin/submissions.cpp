#include "handlers/admin/submissions.hpp"
#include "handlers/admin/template_helper.hpp"
namespace examvan::handlers::admin {
Response submissions_page(const Request&){
  std::string html=render_admin_template("submissions","2.7.2");
  if(!html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Submissions</h1></body></html>"; return r;
}
Response list_submissions(const Request&){ Response r; r.json(200,"{\"success\":true,\"submissions\":[],\"total\":0}"); return r; }
Response submission_detail(const Request&){ Response r; r.json(200,"{\"success\":true,\"submission\":null}"); return r; }
Response queue_status(const Request&){ Response r; r.json(200,"{\"success\":true,\"pending\":0,\"failed\":0}"); return r; }
Response delete_submission(const Request&){ Response r; r.json(200,"{\"success\":true,\"ok\":true}"); return r; }
} // namespace examvan::handlers::admin
