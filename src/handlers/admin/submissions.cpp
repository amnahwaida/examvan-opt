#include "handlers/admin/submissions.hpp"
#include "handlers/admin/template_helper.hpp"
#include "middleware/protobuf.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
namespace examvan::handlers::admin {
Response submissions_page(const Request&){
  std::string html=render_admin_template("submissions","2.7.2");
  if(!html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Submissions</h1></body></html>"; return r;
}
Response list_submissions(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::SubmissionList pb;
    pb.set_success(true);
    pb.set_total(0);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"submissions\":[],\"total\":0}"); return r;
}
Response submission_detail(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::SubmissionDetail pb;
    pb.set_success(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"submission\":null}"); return r;
}
Response queue_status(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::QueueStatusResponse pb;
    pb.set_success(true);
    pb.set_pending(0);
    pb.set_failed(0);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"pending\":0,\"failed\":0}"); return r;
}
Response delete_submission(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::DeleteSubmissionResponse pb;
    pb.set_success(true);
    pb.set_ok(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true}"); return r;
}
} // namespace examvan::handlers::admin
