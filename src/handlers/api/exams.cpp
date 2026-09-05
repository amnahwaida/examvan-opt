#include "handlers/api/exams.hpp"
#include "middleware/version.hpp"
#include "middleware/protobuf.hpp"
#include "helpers/utils.hpp"
#include "models/exam.hpp"
#include "store/exam_store.hpp"
#include "services/examtoken/examtoken.hpp"
#include "handlers/r2/r2.hpp"
#include "config/config.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#ifdef HAS_HIREDIS
#include "redis/redis_real.hpp"
#include <hiredis/hiredis.h>
#endif
#ifdef HAS_LIBPQ
#include "db/pool.hpp"
#include "db/pool_real.hpp"
#endif
#include <string>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <limits>
#include <functional>
#include <map>

namespace examvan::handlers::api {

// Test-only hook: tangkap SubmissionJob yang di-enqueue oleh submit_exam.
static std::function<void(const queue::SubmissionJob&)> g_enqueue_hook;
void set_submit_enqueue_hook_for_test(std::function<void(const queue::SubmissionJob&)> hook){
  g_enqueue_hook = std::move(hook);
}

// Ambil nilai string dari JSON body (key: "x":"value").
static std::string json_string_field(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t p=body.find(needle);
  if(p==std::string::npos) return "";
  size_t colon=body.find(':',p+needle.size());
  if(colon==std::string::npos) return "";
  size_t s=body.find_first_not_of(" \t\r\n",colon+1);
  if(s==std::string::npos || body[s]!='"') return "";
  ++s;
  std::string out;
  for(size_t i=s;i<body.size();++i){
    if(body[i]=='\\' && i+1<body.size()){ out.push_back(body[++i]); continue; }
    if(body[i]=='"') return out;
    out.push_back(body[i]);
  }
  return "";
}

// Ambil nilai JSON mentah (string ber-quote / angka / {objek} / [array]) untuk sebuah key.
static std::string json_raw_value(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t n=body.size();
  bool in_str=false, esc=false;
  for(size_t i=0;i<n;){
    if(!in_str && !esc && i+needle.size()<=n && body.compare(i,needle.size(),needle)==0){
      size_t colon=i+needle.size();
      while(colon<n && (body[colon]==' '||body[colon]=='\t'||body[colon]=='\n'||body[colon]=='\r')) colon++;
      if(colon<n && body[colon]==':'){
        size_t v=colon+1;
        while(v<n && (body[v]==' '||body[v]=='\t'||body[v]=='\n'||body[v]=='\r')) v++;
        if(v>=n) return "";
        if(body[v]=='"'){
          size_t e=v+1; while(e<n){ if(body[e]=='\\'){e+=2;continue;} if(body[e]=='"') break; e++; }
          if(e>=n) return "";
          return body.substr(v,e-v+1);
        }
        if(body[v]=='{' || body[v]=='['){
          char open=body[v], close=(open=='{')?'}':']';
          int depth=0; size_t e=v;
          bool is=false, es=false;
          for(; e<n; ++e){
            char c=body[e];
            if(es){ es=false; continue; }
            if(c=='\\' && is){ es=true; continue; }
            if(c=='"'){ is=!is; continue; }
            if(is) continue;
            if(c==open) depth++;
            else if(c==close){ depth--; if(depth==0) break; }
          }
          if(e>=n || depth!=0) return "";
          return body.substr(v,e-v+1);
        }
        size_t e=v; while(e<n && body[e]!=',' && body[e]!='}' && body[e]!=']') e++;
        return body.substr(v,e-v);
      }
    }
    char c=body[i];
    if(esc){ esc=false; }
    else if(c=='\\' && in_str){ esc=true; }
    else if(c=='"'){ in_str=!in_str; }
    i++;
  }
  return "";
}

// Parse {"1":"A","2":"B"} → map. Dipakai untuk answers/identity_data.
static std::map<std::string,std::string> parse_string_map(const std::string& raw){
  std::map<std::string,std::string> out;
  if(raw.empty() || raw.front()!='{') return out;
  size_t i=1;
  while(i<raw.size()){
    while(i<raw.size() && (raw[i]==' '||raw[i]=='\t'||raw[i]=='\n'||raw[i]=='\r'||raw[i]==',')) i++;
    if(i>=raw.size() || raw[i]=='}') break;
    if(raw[i]!='"'){ i++; continue; }
    size_t e=i+1; while(e<raw.size()){ if(raw[e]=='\\'){e+=2;continue;} if(raw[e]=='"')break; e++; }
    if(e>=raw.size()) break;
    std::string key=raw.substr(i+1,e-i-1);
    i=e+1;
    while(i<raw.size() && (raw[i]==' '||raw[i]=='\t'||raw[i]=='\n'||raw[i]=='\r'||raw[i]==':')) i++;
    if(i>=raw.size() || raw[i]!='"'){ i++; continue; }
    size_t v=i+1; while(v<raw.size()){ if(raw[v]=='\\'){v+=2;continue;} if(raw[v]=='"')break; v++; }
    if(v>=raw.size()) break;
    out[key]=raw.substr(i+1,v-i-1);
    i=v+1;
  }
  return out;
}

// Enqueue SubmissionJob ke Redis (frozen key examvan:submissions:pending).
static void enqueue_job_to_redis(const queue::SubmissionJob& job){
#ifdef HAS_HIREDIS
  auto cfg=Config::load();
  auto ctx=examvan::redis_real::connect_redis(cfg.redis_url);
  if(ctx){
    std::string payload=job.to_json();
    auto* r=(redisReply*)redisCommand(ctx.get(),"LPUSH %s %b", queue::kQueueKey, payload.data(), payload.size());
    if(r) freeReplyObject(r);
  }
#else
  (void)job;
#endif
}

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
    // Parse query components exactly (avoid matching `xpage=`), and accept
    // percent-encoded names/values consistently with form parsing.
    auto decode=[](const std::string& raw){
      std::string out;
      for(size_t i=0;i<raw.size();++i){
        if(raw[i]=='+'){ out.push_back(' '); continue; }
        if(raw[i]=='%' && i+2<raw.size() && std::isxdigit((unsigned char)raw[i+1]) && std::isxdigit((unsigned char)raw[i+2])){
          auto hex=[](char c)->int { if(c>='0'&&c<='9') return c-'0'; c=std::tolower((unsigned char)c); return c-'a'+10; };
          out.push_back(static_cast<char>((hex(raw[i+1])<<4)|hex(raw[i+2]))); i+=2;
        } else out.push_back(raw[i]);
      }
      return out;
    };
    size_t start=0;
    while(start<=req.query.size()){
      size_t end=req.query.find('&',start);
      std::string part=req.query.substr(start,end==std::string::npos?end-start:end-start);
      size_t eq=part.find('=');
      if(eq!=std::string::npos && decode(part.substr(0,eq))==key){
        try {
          size_t used=0;
          const std::string value_text=decode(part.substr(eq+1));
          const int value=std::stoi(value_text,&used);
          if(used!=value_text.size() || value<=0) return fallback;
          return value;
        } catch(...) { return fallback; }
      }
      if(end==std::string::npos) break;
      start=end+1;
    }
    return fallback;
  };
  page=parse_positive("page",1);
  per_page=std::min(parse_positive("per_page",50),200);
  const int total=static_cast<int>(exams.size());
  const int total_pages=total==0?0:(total+per_page-1)/per_page;
  const int64_t offset=static_cast<int64_t>(page-1)*static_cast<int64_t>(per_page);
  const int begin=static_cast<int>(std::min<int64_t>(total,offset));
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
  std::string token=json_string_field(req.body,"token");
  if(token.empty()){
    auto form=helpers::parse_form(req.body);
    if(form.count("token")) token=form["token"];
  }
  if(token.empty()){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"token required\"}"); return r;
  }
  // Validasi: token harus milik exam yang ada (bukan sukses palsu).
  auto snapshot=store::active_store()->list_all();
  bool found=false;
  for(auto& e: snapshot){ if(examtoken::matches(e, token)){ found=true; break; } }
  if(!found){
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::RequestApprovalResponse pb;
      pb.set_success(false);
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=404; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"token not found\"}"); return r;
  }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::RequestApprovalResponse pb;
    pb.set_success(true);
    pb.set_status("pending");
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"status\":\"pending\"}"); return r;
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
  if(it==req.params.end() || it->second.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"exam id required\"}"); return r; }
  int exam_id=0;
  try{ exam_id=std::stoi(it->second); }catch(...){ Response r; r.status=404; r.json(404,"{\"error\":\"exam not found\"}"); return r; }
  auto exam=store::active_store()->get_by_id(exam_id);
  if(!exam){ Response r; r.status=404; r.json(404,"{\"error\":\"exam not found\"}"); return r; }
  if(exam->file_path.empty()){ Response r; r.status=404; r.json(404,"{\"error\":\"file not found\"}"); return r; }
  auto cfg=Config::load();
  r2::R2Config rc{cfg.r2_access_key, cfg.r2_secret_key, cfg.r2_endpoint, cfg.r2_bucket};
  if(!rc.enabled()){
    Response r; r.status=503; r.json(503,"{\"error\":\""+std::string(r2::kErrNotConfigured)+"\",\"error_code\":\""+std::string(r2::kCodeNotConfigured)+"\"}"); return r;
  }
  std::string key=r2::object_key_for_exam(exam_id, exam->file_path);
  std::string url=r2::presign_url(rc, key, 3600);
  if(url.empty()){
    Response r; r.status=503; r.json(503,"{\"error\":\""+std::string(r2::kErrSignFailed)+"\",\"error_code\":\""+std::string(r2::kCodeSignFailed)+"\"}"); return r;
  }
  Response r; r.status=302; r.headers["Location"]=url; return r;
}

Response submit_exam(const Request& req){
  auto it=req.params.find("exam_id");
  if(it==req.params.end() || it->second.empty()){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"exam id required\"}"); return r;
  }
  int exam_id=0;
  try{ exam_id=std::stoi(it->second); }catch(...){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid exam id\"}"); return r;
  }
  auto exam=store::active_store()->get_by_id(exam_id);
  if(!exam){
    Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r;
  }
  // Go parity: submit hanya valid bila ujian aktif & sudah dimulai.
  if(!exam->is_active() || !exam->exam_started_at.has_value() || exam->exam_started_at->empty()){
    Response r; r.status=403; r.json(403,"{\"success\":false,\"error\":\"exam not started\",\"message\":\"Ujian belum dimulai\"}"); return r;
  }
  // Bangun job nyata (identitas + jawaban) lalu enqueue ke queue Redis.
  queue::SubmissionJob job;
  // job_id WAJIB diisi di sini (bukan hanya di SubmissionQueue::enqueue):
  // hook test & jalur Redis LPUSH memakai to_json() langsung, dan result
  // worker di-key per job_id. Tanpa id, semua result menimpa key yang sama.
  job.job_id=queue::generate_job_id();
  job.exam_id=exam_id;
  job.student_name=json_string_field(req.body,"student_name");
  job.exam_number=json_string_field(req.body,"exam_number");
  job.student_class=json_string_field(req.body,"student_class");
  job.mac_address=json_string_field(req.body,"mac_address");
  job.answers=parse_string_map(json_raw_value(req.body,"answers"));
  job.identity_data=parse_string_map(json_raw_value(req.body,"identity_data"));
  job.enqueued_at=helpers::format_iso_utc(std::chrono::system_clock::now());
  if(job.student_name.empty()){
    auto form=helpers::parse_form(req.body);
    if(form.count("student_name")) job.student_name=form["student_name"];
    if(form.count("exam_number")) job.exam_number=form["exam_number"];
    if(form.count("student_class")) job.student_class=form["student_class"];
    if(form.count("mac_address")) job.mac_address=form["mac_address"];
  }
  if(g_enqueue_hook){
    g_enqueue_hook(job);
  } else {
    enqueue_job_to_redis(job);
  }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::SubmitExamResponse pb;
    pb.set_success(true);
    pb.set_status("queued");
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=202; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.status=202; r.json(202,"{\"success\":true,\"status\":\"queued\"}"); return r;
}

Response exam_result(const Request& req){
  auto it=req.params.find("exam_id");
  if(it==req.params.end() || it->second.empty()){
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::ExamResultResponse pb;
      pb.set_success(false);
      pb.set_error("exam id required");
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=400; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"exam id required\"}"); return r;
  }
  int exam_id=0;
  try{ exam_id=std::stoi(it->second); }catch(...){}
  // Validasi: exam harus ada (bukan sukses palsu untuk id sembarang).
  if(!store::active_store()->get_by_id(exam_id).has_value()){
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::ExamResultResponse pb;
      pb.set_success(false);
      pb.set_error("exam not found");
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=404; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r;
  }
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
  Response r; r.json(200,"{\"success\":true,\"exam_id\":"+std::to_string(exam_id)+",\"score\":null,\"has_score\":false}"); return r;
}

Response access_log(const Request& req){
  auto it=req.params.find("exam_id");
  if(it==req.params.end() || it->second.empty()){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"exam id required\"}"); return r;
  }
  int exam_id=0;
  try{ exam_id=std::stoi(it->second); }catch(...){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid exam id\"}"); return r;
  }
  if(!store::active_store()->get_by_id(exam_id).has_value()){
    Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r;
  }
  // Persist akses ke tabel access_log (best-effort; skema dimiliki Go).
  // Gagal insert TIDAK menggagalkan response — response tetap 200 logged.
#ifdef HAS_LIBPQ
  try{
    auto cfg_db=Config::load();
    examvan::DbPool pool(cfg_db.database_url, 10);
    examvan::db::RealPool real(pool.sanitized_url(), 10);
    if(auto c=real.acquire()){
      std::string mac=json_string_field(req.body,"mac_address");
      std::string sname=json_string_field(req.body,"student_name");
      std::string snum=json_string_field(req.body,"exam_number");
      std::string sclass=json_string_field(req.body,"student_class");
      std::string event=json_string_field(req.body,"event");
      std::string device=json_string_field(req.body,"device_info");
      if(mac.empty()){
        auto form=helpers::parse_form(req.body);
        if(form.count("mac_address")) mac=form["mac_address"];
        if(form.count("student_name")) sname=form["student_name"];
        if(form.count("exam_number")) snum=form["exam_number"];
        if(form.count("student_class")) sclass=form["student_class"];
        if(form.count("event")) event=form["event"];
        if(form.count("device_info")) device=form["device_info"];
      }
      if(event.empty()) event="login"; // Go: AccessEventLogin
      std::string ip=req.headers.count("X-Forwarded-For")?req.headers["X-Forwarded-For"]:"";
      std::string identity=json_raw_value(req.body,"identity_data");
      if(identity.empty()){ auto form=helpers::parse_form(req.body); if(form.count("identity_data")) identity=form["identity_data"]; }
      // Paritas Go: heartbeat TIDAK ditulis ke DB (Redis-only, hemat IO).
      // Kolom = schema Go student_access_logs (student_identifier = mac).
      if(event!="heartbeat"){
        real.exec_params(c.get(),
          "INSERT INTO student_access_logs (exam_id, student_identifier, student_name, exam_number, student_class, event, ip_address, device_info, identity_data)"
          " VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)",
          {std::to_string(exam_id), mac, sname, snum, sclass, event, ip, device, identity});
      }
      real.release(c.release());
    }
  }catch(...){ /* best-effort: jangan sampai access-log mematikan handler */ }
#endif
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::AccessLogResponse pb;
    pb.set_success(true);
    pb.set_logged(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"logged\":true}"); return r;
}

Response complete_exam(const Request& req){
  auto it=req.params.find("exam_id");
  if(it==req.params.end() || it->second.empty()){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"exam id required\"}"); return r;
  }
  int exam_id=0;
  try{ exam_id=std::stoi(it->second); }catch(...){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid exam id\"}"); return r;
  }
  if(!store::active_store()->get_by_id(exam_id).has_value()){
    Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r;
  }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::CompleteExamResponse pb;
    pb.set_success(true);
    pb.set_completed(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"completed\":true}"); return r;
}

} // namespace examvan::handlers::api
