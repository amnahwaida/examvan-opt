#include "handlers/api/exams.hpp"
#include "middleware/version.hpp"
#include "middleware/protobuf.hpp"
#include "helpers/utils.hpp"
#include "models/exam.hpp"
#include "store/exam_store.hpp"
#include "services/examtoken/examtoken.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#include <string>
#include <algorithm>
#include <cstdlib>

namespace examvan::handlers::api {

static std::string json_escape(const std::string& s){
  std::string o; o.reserve(s.size()+16);
  for(unsigned char c: s){
    switch(c){
      case '"': o+="\\\""; break;
      case '\\': o+="\\\\"; break;
      case '\b': o+="\\b"; break;
      case '\f': o+="\\f"; break;
      case '\n': o+="\\n"; break;
      case '\r': o+="\\r"; break;
      case '\t': o+="\\t"; break;
      default:
        if(c<0x20){ char buf[7]; snprintf(buf,sizeof(buf),"\\u%04x",c); o+=buf; }
        else o+=char(c);
    }
  }
  return o;
}

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
  const auto exams=store::active_store()->list_all();
  int page=1;
  int per_page=50;
  auto parse_positive=[&](const char* key, int fallback){
    const std::string needle=std::string(key)+"=";
    size_t pos=req.query.find(needle);
    if(pos==std::string::npos) return fallback;
    pos+=needle.size();
    size_t end=req.query.find('&',pos);
    try {
      const int value=std::stoi(req.query.substr(pos,end==std::string::npos?end:end-pos));
      return value>0?value:fallback;
    } catch(...) { return fallback; }
  };
  page=parse_positive("page",1);
  per_page=std::min(parse_positive("per_page",50),200);
  const int total=static_cast<int>(exams.size());
  const int total_pages=total==0?0:(total+per_page-1)/per_page;
  const int begin=std::min(total,(page-1)*per_page);
  const int end=std::min(total,begin+per_page);
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::ListExamsResponse pb;
    pb.set_success(true);
    pb.set_page(page);
    pb.set_per_page(per_page);
    pb.set_total(total);
    pb.set_total_pages(total_pages);
    for(int i=begin;i<end;++i){
      pb.add_tokens(exams[i].active_token.empty()?exams[i].token:exams[i].active_token);
    }
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  std::string data="[";
  for(int i=begin;i<end;++i){
    if(i>begin) data+=",";
    const auto& e=exams[i];
    data+="{\"id\":"+std::to_string(e.id)+
      ",\"name\":\""+json_escape(e.name)+"\""+
      ",\"token\":\""+json_escape(e.token)+"\""+
      ",\"active_token\":\""+json_escape(e.active_token.empty()?e.token:e.active_token)+"\""+
      ",\"file_path\":\""+json_escape(e.file_path)+"\""+
      ",\"status\":\""+json_escape(e.status)+"\""+
      ",\"created_at\":\""+json_escape(e.created_at)+"\"}";
  }
  data+="]";
  Response r; r.json(200,"{\"data\":"+data+",\"pagination\":{\"page\":"+
    std::to_string(page)+",\"per_page\":"+std::to_string(per_page)+
    ",\"total\":"+std::to_string(total)+",\"total_pages\":"+
    std::to_string(total_pages)+"},\"success\":true}");
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
  std::string token=it->second;
  // Lookup berdasarkan token atau active_token dari store.
  // Go parity: static menerima permanent token sebagai fallback; dynamic hanya active_token.
  store::ExamStore& st=*store::active_store();
  auto snapshot=st.list_all();
  const models::Exam* matched=nullptr;
  for(auto& e: snapshot){
    if(!examtoken::matches(e, token)) continue;
    matched=&e; break;
  }
  if(!matched){
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
  // Copy the matched snapshot before any mutation. Go's
  // MaybeResetActiveToken rotates lazily on the join path; the in-memory
  // store update holds its mutex for the complete mutation.
  models::Exam exam=*matched;
  if(exam.get_token_mode()=="dynamic" &&
     exam.exam_started_at.has_value() && !exam.exam_started_at->empty() &&
     exam.token_reset_interval.has_value() && *exam.token_reset_interval>0){
    const std::string reset_ref=exam.token_last_reset_at.value_or(*exam.exam_started_at);
    const auto reset_at=helpers::parse_iso_utc(reset_ref);
    const auto now=std::chrono::system_clock::now();
    if(reset_at && now >= *reset_at + std::chrono::minutes(*exam.token_reset_interval)){
      const std::string new_token=helpers::generate_token(8);
      const std::string now_text=helpers::format_iso_utc(now);
      const bool rotated=st.update(exam.id, [&](models::Exam& current){
        // Recheck under the store lock so concurrent joins produce at most
        // one rotation for the same interval.
        const std::string current_ref=current.token_last_reset_at.value_or(
          current.exam_started_at.value_or(std::string{}));
        const auto current_reset=helpers::parse_iso_utc(current_ref);
        if(current.get_token_mode()=="dynamic" &&
           current.exam_started_at.has_value() && current.token_reset_interval.has_value() &&
           *current.token_reset_interval>0 && current_reset &&
           now >= *current_reset + std::chrono::minutes(*current.token_reset_interval)){
          current.active_token=new_token;
          current.token_last_reset_at=now_text;
          exam.active_token=new_token;
          exam.token_last_reset_at=now_text;
        }
      });
      (void)rotated;
    }
  }
  if(!exam.is_active() || !exam.exam_started_at.has_value() || exam.exam_started_at->empty()){
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::ExamByTokenResponse pb;
      pb.set_success(false);
      pb.set_error("exam not started");
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=403; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    Response r; r.status=403; r.json(403,"{\"success\":false,\"error\":\"exam not started\",\"message\":\"Ujian belum dimulai\"}"); return r;
  }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::ExamByTokenResponse pb;
    pb.set_success(true);
    pb.set_token(token);
    pb.set_status(exam.status);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  std::string esc=json_escape(token);
  Response r; r.status=200; r.json(200,"{\"token\":\""+esc+"\",\"status\":\""+exam.status+"\",\"id\":"+std::to_string(exam.id)+",\"name\":\""+json_escape(exam.name)+"\",\"success\":true}"); return r;
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
