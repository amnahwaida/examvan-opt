#include "handlers/api/exams.hpp"
#include "middleware/version.hpp"
#include "middleware/protobuf.hpp"
#include "helpers/utils.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#include <string>

namespace examvan::handlers::api {

Response health(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::HealthResponse pb;
    pb.set_status("healthy");
    pb.set_version("2.7.2");
    pb.set_uwebsockets(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,
    "{\"certificate_fingerprint\":\"\","
    "\"required_app_version\":\"\","
    "\"server_time_utc\":\""+helpers::format_iso_utc(std::chrono::system_clock::now())+"\","
    "\"status\":\"healthy\","
    "\"status\":\"ok\","
    "\"success\":true,"
    "\"version\":\"2.7.2\"}");
  return r;
}

Response time_handler(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::TimeResponse pb;
    pb.set_success(true);
    pb.set_server_time(helpers::format_iso_utc(std::chrono::system_clock::now()));
    pb.set_timezone("UTC");
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"server_time\":\""+helpers::format_iso_utc(std::chrono::system_clock::now())+"\",\"success\":true,\"timezone\":\"UTC\"}"); return r;
}

Response list_exams(const Request& req){
  /* Semantik AndroidVersionCheck Go (version.go):
   * 1) header X-App-Version KOSONG  → izinkan (client web)
   * 2) required versi KOSONG        → izinkan (belum ada APK terbit)
   * 3) header ada + required ada    → bandingkan, 426 + pesan Go bila tua */
  auto v=req.headers.find("X-App-Version");
  std::string cv=v!=req.headers.end()?v->second:"";
  const std::string required=""; // paritas fresh-DB; wiring saas_settings menyusul
  if(middleware::should_block_version(cv,required)){
    Response r; r.status=426; r.json(426,
      "{\"success\":false,\"message\":\"Versi aplikasi Anda ("+cv+") sudah tidak didukung. "
      "Silakan download versi terbaru ("+required+") dari halaman Download.\"}");
    return r;
  }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::ListExamsResponse pb;
    pb.set_success(true);
    pb.set_page(1);
    pb.set_per_page(50);
    pb.set_total(0);
    pb.set_total_pages(0);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"data\":[],\"pagination\":{\"page\":1,\"per_page\":50,\"total\":0,\"total_pages\":0},\"success\":true}");
  return r;
}

Response request_approval(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::RequestApprovalResponse pb;
    pb.set_success(true);
    pb.set_status("pending");
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"status\":\"pending\"}"); return r;
}

Response exam_by_token(const Request& req){
  auto it=req.params.find("token");
  if(it==req.params.end()||!helpers::is_valid_exam_token(it->second)){
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::ExamByTokenResponse pb;
      pb.set_success(false);
      pb.set_error("token not found");
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=404; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    Response r; r.status=404; r.json(404,"{\"error\":\"token not found\"}"); return r;
  }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::ExamByTokenResponse pb;
    pb.set_success(true);
    pb.set_token(it->second);
    pb.set_status("active");
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"token\":\""+it->second+"\",\"status\":\"active\"}"); return r;
}

Response exam_pdf(const Request& req){
  auto it=req.params.find("exam_id");
  if(it==req.params.end()){ Response r; r.status=404; r.json(404,"{\"error\":\"not found\"}"); return r;}
  Response r; r.status=302; r.headers["Location"]="https://r2.example.com/bucket/exams/"+it->second+"/paper.pdf?presigned=1"; return r;
}

Response submit_exam(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::SubmitExamResponse pb;
    pb.set_success(true);
    pb.set_status("queued");
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=202; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.status=202; r.json(202,"{\"status\":\"queued\"}"); return r;
}

Response exam_result(const Request& req){
  auto it=req.params.find("exam_id");
  if(it==req.params.end()){
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::ExamResultResponse pb;
      pb.set_success(false);
      pb.set_error("not found");
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=404; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    Response r; r.status=404; r.json(404,"{\"error\":\"not found\"}"); return r;
  }
  int exam_id=0;
  try{ exam_id=std::stoi(it->second); }catch(...){}
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::ExamResultResponse pb;
    pb.set_success(true);
    pb.set_exam_id(exam_id);
    pb.set_has_score(false);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"exam_id\":"+it->second+",\"score\":null}"); return r;
}

Response access_log(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::AccessLogResponse pb;
    pb.set_success(true);
    pb.set_logged(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"logged\":true}"); return r;
}

Response complete_exam(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::CompleteExamResponse pb;
    pb.set_success(true);
    pb.set_completed(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"completed\":true}"); return r;
}

} // namespace examvan::handlers::api
