#include "handlers/admin/exams.hpp"
#include "helpers/utils.hpp"
#include "config/config.hpp"
#include "handlers/r2/r2.hpp"
#include "middleware/protobuf.hpp"
#include "utils/log.hpp"
#include "store/exam_store.hpp"
#include <chrono>
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#include <cctype>
#include <vector>
#include <functional>
#include <algorithm>
namespace examvan::handlers::admin {
using namespace examvan::utils;
static std::string get_param(const std::map<std::string,std::string>& form, const std::string& k){
  auto it=form.find(k); return it!=form.end()? it->second : "";
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
static bool parse_multipart(const std::string& body, const std::string& ct,
                            std::map<std::string,std::string>& fields,
                            std::string& out_filename, std::string& out_filedata, std::string& out_filect){
  size_t bpos=ct.find("boundary=");
  if(bpos==std::string::npos) return false;
  std::string boundary=ct.substr(bpos+9);
  if(!boundary.empty() && boundary.front()=='"') { boundary=boundary.substr(1); auto e=boundary.find('"'); if(e!=std::string::npos) boundary=boundary.substr(0,e); }
  // trim
  {
    size_t s=boundary.find_first_not_of(" \t\r\n");
    size_t e=boundary.find_last_not_of(" \t\r\n");
    if(s!=std::string::npos) boundary=boundary.substr(s,e-s+1);
  }
  if(boundary.empty()) return false;
  std::string delim="--"+boundary;
  size_t pos=0;
  while(true){
    size_t d=body.find(delim,pos);
    if(d==std::string::npos) break;
    size_t head_start=d+delim.size();
    if(body.compare(head_start,2,"--")==0) break;
    if(body.compare(head_start,2,"\r\n")==0) head_start+=2;
    else if(head_start<body.size() && body[head_start]=='\n') head_start+=1;
    else break;
    size_t hdr_end=body.find("\r\n\r\n",head_start);
    if(hdr_end==std::string::npos) break;
    std::string hdr=body.substr(head_start, hdr_end-head_start);
    size_t data_start=hdr_end+4;
    size_t next=body.find(delim,data_start);
    if(next==std::string::npos) break;
    size_t data_end=next;
    if(data_end>=2 && body.compare(data_end-2,2,"\r\n")==0) data_end-=2;
    std::string data=body.substr(data_start, data_end-data_start);
    // parse disposition
    std::string name, filename, ct_part;
    // name
    {
      size_t p=hdr.find("name=\"");
      if(p!=std::string::npos){ p+=6; size_t q=hdr.find('"',p); if(q!=std::string::npos) name=hdr.substr(p,q-p); }
      else { p=hdr.find("name="); if(p!=std::string::npos){ p+=5; size_t q=hdr.find_first_of(";\r\n ",p); if(q==std::string::npos) q=hdr.size(); name=hdr.substr(p,q-p); if(!name.empty() && name.front()=='"') name=name.substr(1); if(!name.empty() && name.back()=='"') name.pop_back(); } }
    }
    {
      size_t p=hdr.find("filename=\"");
      if(p!=std::string::npos){ p+=10; size_t q=hdr.find('"',p); if(q!=std::string::npos) filename=hdr.substr(p,q-p); }
    }
    {
      size_t p=hdr.find("Content-Type:");
      if(p!=std::string::npos){ p+=13; size_t q=hdr.find("\r\n",p); if(q==std::string::npos) q=hdr.size(); ct_part=hdr.substr(p,q-p); size_t s=ct_part.find_first_not_of(" \t"); size_t e=ct_part.find_last_not_of(" \t\r\n"); if(s!=std::string::npos) ct_part=ct_part.substr(s,e-s+1); }
    }
    if(!filename.empty()){
      out_filename=filename;
      out_filedata=data;
      out_filect=ct_part;
    } else if(!name.empty()){
      fields[name]=data;
    }
    pos=next;
  }
  return true;
}
/* Storage: lewat ExamStore abstraction, bukan g_exams langsung.
 * Default = in-memory; test bisa swap via set_active_store(). */
static store::ExamStore& exams(){
  return *store::active_store();
}

/* Test hooks: callback untuk mock upload & token generation */
static std::function<void(const std::string&,const std::string&)> g_upload_mock;
static std::function<std::string(int)> g_token_gen_override;

void set_upload_mock_for_test(std::function<void(const std::string&,const std::string&)> mock){
  g_upload_mock = std::move(mock);
}
void set_token_generator_for_test(std::function<std::string(int)> gen){
  g_token_gen_override = std::move(gen);
}

void clear_exams_for_testing(){
  exams().clear_all();
  g_upload_mock = nullptr;
  g_token_gen_override = nullptr;
}

static std::string gen_token(int len){
  if(g_token_gen_override) return g_token_gen_override(len);
  return helpers::generate_token(len);
}

static bool has_null_bytes(const std::string& s){
  for(char c: s) if(c=='\0') return true;
  return false;
}
Response list_admin_exams(const Request& req){
  auto snapshot = exams().list_all(); // ambil snapshot (thread-safe)
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::AdminExamList pb;
    pb.set_success(true);
    pb.set_total(static_cast<int32_t>(snapshot.size()));
    for(auto &e: snapshot){
      auto *ex=pb.add_exams();
      ex->set_id(e.id);
      ex->set_name(e.name);
      ex->set_token(e.token);
      ex->set_file_path(e.file_path);
      ex->set_size_bytes(e.size_bytes);
      ex->set_status(e.status);
      ex->set_created_at(e.created_at);
    }
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  std::string json="[";
  for(size_t i=0;i<snapshot.size();++i){
    if(i) json+=",";
    auto &e=snapshot[i];
    json+="{\"id\":"+std::to_string(e.id)+",\"name\":\""+json_escape(e.name)+"\",\"token\":\""+json_escape(e.token)+"\",\"file_path\":\""+json_escape(e.file_path)+"\",\"size_bytes\":"+std::to_string(e.size_bytes)+",\"status\":\""+json_escape(e.status)+"\",\"created_at\":\""+json_escape(e.created_at)+"\"}";
  }
  json+="]";
  Response r; r.json(200,"{\"success\":true,\"exams\":"+json+",\"total\":"+std::to_string(snapshot.size())+"}"); return r;
}
Response create_exam(const Request& req){
  std::map<std::string,std::string> form;
  std::string file_name, file_data, file_ct;
  std::string ct;
  auto itct=req.headers.find("Content-Type");
  if(itct!=req.headers.end()) ct=itct->second;
  else {
    for(auto &kv: req.headers){ std::string low=kv.first; for(char &c: low) c=tolower((unsigned char)c); if(low=="content-type"){ ct=kv.second; break; } }
  }
  auto cfg_pb = Config::load();
  bool is_pb = middleware::is_protobuf_content(req);
  bool is_multipart_pb = ct.find("multipart/form-data")!=std::string::npos;
  if(cfg_pb.protobuf_mandatory && !is_pb && !is_multipart_pb && !req.body.empty()){
    if(auto err = middleware::require_protobuf(req, cfg_pb); err) return *err;
  }
  std::string name, fpath, sz, custom;
  if(is_pb){
#ifdef HAS_PROTOBUF
    examvan::v1::CreateExamRequest pb;
    if(!pb.ParseFromArray(req.body.data(), req.body.size())){
      Response r; r.status=400; r.json(400,"{\"error\":\"invalid protobuf\"}"); return r;
    }
    name = pb.name();
    fpath = pb.file_path();
    if(pb.size_bytes()!=0) sz = std::to_string(pb.size_bytes());
    custom = pb.custom_token();
    file_data = pb.pdf_data();
    if(!file_data.empty() && fpath.empty()) fpath = "upload.pdf";
    if(!file_data.empty()) file_name = fpath;
#else
    Response r; r.status=415; r.json(415,"{\"error\":\"protobuf not enabled\",\"error_code\":\"PROTOBUF_REQUIRED\"}"); return r;
#endif
  } else {
  bool is_multipart = is_multipart_pb;
  if(is_multipart){
    parse_multipart(req.body, ct, form, file_name, file_data, file_ct);
  } else {
    form=helpers::parse_form(req.body);
    if(form.empty() && !req.body.empty() && req.body.find('{')!=std::string::npos){
      auto jf=[&](const std::string& k)->std::string{
        std::string needle="\""+k+"\"";
        size_t p=req.body.find(needle);
        if(p==std::string::npos) return "";
        p=req.body.find(':',p); if(p==std::string::npos) return "";
        size_t s=req.body.find_first_not_of(" \t\r\n",p+1);
        if(s==std::string::npos) return "";
        if(req.body[s]=='"'){
          size_t e=s+1; while(e<req.body.size()){ if(req.body[e]=='\\'){e+=2;continue;} if(req.body[e]=='"') break; e++; }
          if(e>=req.body.size()) return "";
          return req.body.substr(s+1,e-s-1);
        } else {
          size_t e=req.body.find_first_of(",}",s);
          if(e==std::string::npos) e=req.body.size();
          std::string v=req.body.substr(s,e-s);
          size_t a=v.find_first_not_of(" \t\r\n\""); size_t b=v.find_last_not_of(" \t\r\n\"");
          if(a!=std::string::npos) v=v.substr(a,b-a+1);
          return v;
        }
      };
      std::string jn=jf("name"); if(!jn.empty()) form["name"]=jn;
      std::string jf2=jf("file_path"); if(!jf2.empty()) form["file_path"]=jf2;
      std::string jt=jf("custom_token"); if(!jt.empty()) form["custom_token"]=jt;
      std::string js=jf("size_bytes"); if(!js.empty()) form["size_bytes"]=js;
    }
  }
  name = get_param(form,"name");
  fpath = get_param(form,"file_path");
  sz = get_param(form,"size_bytes");
  custom = get_param(form,"custom_token");
  if(!file_name.empty()){
    fpath=file_name;
    if(sz.empty()) sz=std::to_string(file_data.size());
  }
  }
  // name sanitasi & validasi
  {
    std::string trimmed=helpers::sanitize_student_input(name);
    // keep original for length check after sanitize? use trimmed for storage
    name=trimmed;
  }
  if(name.empty()){ utils::log_error("exam_create_failed","reason=name_required"); Response r; r.status=400; r.json(400,"{\"error\":\"name required\"}"); return r; }
  if(name.size()>255){ Response r; r.status=400; r.json(400,"{\"error\":\"name too long\"}"); return r; }
  if(fpath.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"file_path required\"}"); return r; }
  // file_path hardening: null-byte, traversal bertingkat, backslash
  if(has_null_bytes(fpath) || fpath.find("..")!=std::string::npos || fpath.find("\\")!=std::string::npos){
    Response r; r.status=400; r.json(400,"{\"error\":\"file_path must not contain traversal\"}"); return r;
  }
  // MIME check for multipart pdf - require %PDF magic (content-type can be spoofed)
  if(is_multipart_pb && !file_data.empty()){
    bool is_pdf_magic = file_data.rfind("%PDF",0)==0;
    if(!is_pdf_magic){ Response r; r.status=400; r.json(400,"{\"error\":\"file must be PDF\"}"); return r; }
  }
  // size validation — sebelum R2 upload (hemat bandwidth)
  long size=0;
  try{ if(!sz.empty()) size=std::stol(sz); else if(!file_data.empty()) size=file_data.size(); }catch(...){}
  const long MAX_PDF = 5*1024*1024;
  if(size>MAX_PDF){ Response r; r.status=413; r.json(413,"{\"error\":\"file too large, max 5MB\"}"); return r; }
  // custom_token validasi & collision check
  std::string token;
  if(!custom.empty()){
    for(char &c: custom) c=toupper((unsigned char)c);
    if(!helpers::is_valid_exam_token(custom) || custom.size()!=8){
      Response r; r.status=400; r.json(400,"{\"error\":\"custom_token must be 8 A-Z0-9\"}"); return r;
    }
    // custom token harus unik terhadap semua exam existing
    token=custom; // assign SEBELUM collision check supaya perbandingan benar
    if(exams().token_exists(token)){
      Response r; r.status=409; r.json(409,"{\"error\":\"custom_token already in use\",\"error_code\":\"DUPLICATE_TOKEN\"}"); return r;
    }
  } else {
    // generate unique — error eksplisit jika seluruh attempt collide
    bool found=false;
    for(int tries=0;tries<5;tries++){
      token=gen_token(8);
      if(exams().claim_token(token)){ found=true; break; }
    }
    if(!found){
      utils::log_error("exam_create_failed","reason=token_collision_exhausted");
      Response r; r.status=409; r.json(409,"{\"error\":\"token generation failed after retries\",\"error_code\":\"TOKEN_COLLISION\"}"); return r;
    }
  }
  // Generate ID dulu (sebelum R2 upload, supaya key pakai id nyata)
  int id = exams().next_id();
  // R2 mandatory fail-closed
  {
    auto cfg_r2 = Config::load();
    r2::R2Config rc{cfg_r2.r2_access_key, cfg_r2.r2_secret_key, cfg_r2.r2_endpoint, cfg_r2.r2_bucket};
    if(!file_data.empty() && !rc.enabled()){
      Response r; r.status=503; r.json(503,"{\"error\":\""+std::string(r2::kErrNotConfigured)+"\",\"error_code\":\""+std::string(r2::kCodeNotConfigured)+"\"}"); return r;
    }
    if(!file_data.empty() && rc.enabled()){
      std::string key = r2::object_key_for_exam(id, fpath);
      if(g_upload_mock){
        g_upload_mock(key, file_data);
      } else {
        r2::R2Client client{rc};
        if(!client.upload(key, file_data)){
          Response r; r.status=502; r.json(502,"{\"error\":\""+std::string(r2::kErrUploadFailed)+"\",\"error_code\":\""+std::string(r2::kCodeUploadFailed)+"\"}"); return r;
        }
      }
    }
  }
  // Store via models::Exam (lengkap, bukan triplet)
  {
    models::Exam exam;
    exam.id=id;
    exam.name=name;
    exam.file_path=fpath;
    exam.size_bytes=size;
    exam.token=token;
    exam.status="inactive";
    exam.security_level="medium";
    exam.created_at=helpers::format_iso_utc(std::chrono::system_clock::now());
    exams().add(exam);
  }
  utils::log_info("exam_created","id="+std::to_string(id)+" token="+token+" name="+name);
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::CreateExamResponse pb;
    pb.set_success(true);
    pb.set_id(id);
    pb.set_token(token);
    pb.set_name(name);
    pb.set_file_path(fpath);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=201; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  std::string esc_name=json_escape(name);
  std::string esc_fpath=json_escape(fpath);
  std::string esc_token=json_escape(token);
  Response r; r.status=201; r.json(201,"{\"success\":true,\"id\":"+std::to_string(id)+",\"token\":\""+esc_token+"\",\"name\":\""+esc_name+"\",\"file_path\":\""+esc_fpath+"\",\"status\":\"inactive\",\"size_bytes\":"+std::to_string(size)+",\"created_at\":\""+helpers::format_iso_utc(std::chrono::system_clock::now())+"\"}"); return r;
}
static std::string get_exam_id(const Request& req){
  auto it=req.params.find("id");
  if(it!=req.params.end()) return it->second;
  it=req.params.find("exam_id");
  if(it!=req.params.end()) return it->second;
  return "";
}

Response update_exam(const Request& req){
  auto id_str=get_exam_id(req);
  if(id_str.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"exam id required\"}"); return r; }
  int id=0;
  try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"error\":\"invalid exam id\"}"); return r; }
  std::string action=req.params.count("action")? req.params.at("action") : "";
  // tentukan action dari path jika tidak ada param action
  if(action.empty()){
    auto p=req.path.find("/toggle"); if(p!=std::string::npos){ action="toggle"; }
    else { p=req.path.find("/start"); if(p!=std::string::npos) action="start"; }
    if(action.empty()){ p=req.path.find("/stop"); if(p!=std::string::npos) action="stop"; }
    if(action.empty()){ p=req.path.find("/regenerate-token"); if(p!=std::string::npos) action="regenerate-token"; }
    if(action.empty()){ p=req.path.find("/edit"); if(p!=std::string::npos) action="edit"; }
  }
  // regenerate-token: claim token baru (atomic thd store) lalu terapkan via update()
  if(action=="regenerate-token"){
    std::string new_token;
    bool ok=false;
    for(int tries=0;tries<5;tries++){
      new_token=gen_token(8);
      if(exams().claim_token(new_token)){ ok=true; break; }
    }
    bool found=false;
    if(ok){
      found = exams().update(id, [&](models::Exam& e){ e.token=new_token; });
    } else {
      // claim selalu gagal -> tolak (bukan silent reuse)
      found = exams().get_by_id(id).has_value();
    }
    if(!found){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
    utils::log_info("exam_token_regenerated","id="+id_str);
    auto cur = exams().get_by_id(id);
    std::string shown = cur? cur->token : "";
    Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+",\"token\":\""+json_escape(shown)+"\"}"); return r;
  }
  // semua action lain: mutasi via store.update() (mutator jalan dalam lock store)
  // pre-validasi nama hanya untuk action edit
  std::string new_name;
  if(action=="edit"){
    new_name=req.params.count("name")? req.params.at("name") : "";
    if(new_name.empty()){
      auto form=helpers::parse_form(req.body);
      new_name = form.count("name")? form["name"] : "";
    }
    if(!new_name.empty()){
      new_name=helpers::sanitize_student_input(new_name);
      if(new_name.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"name required\"}"); return r; }
      if(new_name.size()>255){ Response r; r.status=400; r.json(400,"{\"error\":\"name too long\"}"); return r; }
    }
  }
  std::string result_status;
  std::string result_name;
  bool found=false;
  found = exams().update(id, [&](models::Exam& e){
    if(action=="toggle"){ e.status = e.is_active()? "inactive" : "active"; result_status=e.status; }
    else if(action=="start"){ e.status="active"; result_status="active"; }
    else if(action=="stop"){ e.status="inactive"; result_status="inactive"; }
    else if(action=="edit" && !new_name.empty()){ e.name=new_name; result_name=new_name; }
  });
  if(!found){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::UpdateExamResponse pb;
    pb.set_success(true);
    pb.set_ok(true);
    pb.set_id(id);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  if(!result_status.empty()){
    utils::log_info("exam_updated","id="+id_str+" action="+action+" status="+result_status);
    Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+",\"status\":\""+result_status+"\"}"); return r;
  }
  if(!result_name.empty()){
    utils::log_info("exam_updated","id="+id_str+" action=edit name="+result_name);
    Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+",\"name\":\""+json_escape(result_name)+"\"}"); return r;
  }
  // tidak ada action dikenal / tidak ada perubahan -> 400
  Response r; r.status=400; r.json(400,"{\"error\":\"no valid update field\"}"); return r;
}

Response delete_exam(const Request& req){
  auto id_str=get_exam_id(req);
  if(id_str.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"exam id required\"}"); return r; }
  int id=0;
  try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"error\":\"invalid exam id\"}"); return r; }
  if(!exams().remove(id)){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
  utils::log_info("exam_deleted","id="+id_str);
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::DeleteExamResponse pb;
    pb.set_success(true);
    pb.set_ok(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+"}"); return r;
}
Response export_xlsx(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  r.headers["Content-Disposition"]="attachment; filename=\"export.xlsx\"";
  r.body="PK fake xlsx content"; return r;
}
} // namespace examvan::handlers::admin
