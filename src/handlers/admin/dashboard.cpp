#include "handlers/admin/dashboard.hpp"
#include "handlers/admin/template_helper.hpp"
#include "utils/sanitize.hpp"
#include "middleware/protobuf.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
namespace examvan::handlers::admin {
Response dashboard_page(const Request& req){
  std::string html=render_admin_template("dashboard","2.7.2");
  if(!html.empty()){
    auto it=req.headers.find("X-User");
    if(it!=req.headers.end()) html+=html_escape(it->second);
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Dashboard</h1></body></html>"; return r;
}
Response dashboard_stats(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::DashboardStats pb;
    pb.set_success(true);
    pb.set_total(0);
    pb.set_active(0);
    pb.set_storage_mb(0);
    pb.set_exams(0);
    pb.set_submissions(0);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"data\":{\"total\":0,\"active\":0,\"storage_mb\":0,\"exams\":0,\"submissions\":0},\"exams\":0,\"submissions\":0}"); return r;
}
} // namespace examvan::handlers::admin
