#include "handlers/api/exams.hpp"
#include "middleware/version.hpp"
#include "helpers/utils.hpp"
#include <string>

namespace examvan::handlers::api {

Response health(const Request& req){
  (void)req;
  Response r; r.json(200,"{\"status\":\"ok\",\"required_app_version\":\"2.7.2\"}"); return r;
}

Response time_handler(const Request&){
  Response r; r.json(200,"{\"now\":\""+helpers::format_iso_utc(std::chrono::system_clock::now())+"\"}"); return r;
}

Response list_exams(const Request& req){
  auto v=req.headers.find("X-App-Version");
  std::string cv=v!=req.headers.end()?v->second:"";
  if(!middleware::is_version_allowed(cv,"2.7.2")){
    Response r; r.status=426; r.json(426,"{\"error\":\"Versi Aplikasi Kedaluwarsa\"}"); return r;
  }
  Response r; r.json(200,"{\"exams\":[]}");
  return r;
}

Response request_approval(const Request& req){
  (void)req;
  Response r; r.json(200,"{\"status\":\"pending\"}"); return r;
}

Response exam_by_token(const Request& req){
  auto it=req.params.find("token");
  if(it==req.params.end()||!helpers::is_valid_exam_token(it->second)){
    Response r; r.status=404; r.json(404,"{\"error\":\"token not found\"}"); return r;
  }
  Response r; r.json(200,"{\"token\":\""+it->second+"\",\"status\":\"active\"}"); return r;
}

Response exam_pdf(const Request& req){
  auto it=req.params.find("exam_id");
  if(it==req.params.end()){ Response r; r.status=404; r.json(404,"{\"error\":\"not found\"}"); return r;}
  Response r; r.status=302; r.headers["Location"]="https://r2.example.com/bucket/exams/"+it->second+"/paper.pdf?presigned=1"; return r;
}

Response submit_exam(const Request& req){
  (void)req;
  Response r; r.status=202; r.json(202,"{\"status\":\"queued\"}"); return r;
}

Response exam_result(const Request& req){
  auto it=req.params.find("exam_id");
  if(it==req.params.end()){ Response r; r.status=404; r.json(404,"{\"error\":\"not found\"}"); return r;}
  Response r; r.json(200,"{\"exam_id\":"+it->second+",\"score\":null}"); return r;
}

Response access_log(const Request& req){
  (void)req; Response r; r.json(200,"{\"logged\":true}"); return r;
}

Response complete_exam(const Request& req){
  (void)req; Response r; r.json(200,"{\"completed\":true}"); return r;
}

} // namespace examvan::handlers::api
