#include "handlers/admin/exams.hpp"
#include "handlers/admin/export.hpp"
#include "helpers/utils.hpp"
#include "config/config.hpp"
#include "handlers/r2/r2.hpp"
#include "middleware/protobuf.hpp"
#include "utils/log.hpp"
#include "store/exam_store.hpp"
#include "services/examtoken/examtoken.hpp"
#ifdef HAS_LIBPQ
#include "db/pool.hpp"
#include "db/pool_real.hpp"
#include <libpq-fe.h>
#endif
#include <chrono>
#include <cstdio>
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
  // Bug B: boundary tidak ber-quote boleh diikuti param lain dipotong ';'
  // (mis. boundary=abc; charset=utf-8) — potong setelah semicolon pertama.
  {
    auto semi=boundary.find(';');
    if(semi!=std::string::npos) boundary=boundary.substr(0,semi);
  }
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
static std::string idempotency_fingerprint(const Request& req){
  std::string fp=req.method+"\\n"+req.path+"\\n"+req.body;
  return std::to_string(std::hash<std::string>{}(fp))+":"+std::to_string(fp.size());
}

void set_upload_mock_for_test(std::function<void(const std::string&,const std::string&)> mock){
  g_upload_mock = std::move(mock);
}
void set_token_generator_for_test(std::function<std::string(int)> gen){
  g_token_gen_override = std::move(gen);
}

void clear_exams_for_testing(){
  exams().clear_all();           // also clears idempotency state via ExamStore::clear_all()
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

/* Security Gap 1: validasi PDF — magic header + %%EOF marker + polyglot detection.
 * Go reference: validatePDF() — cek %PDF, %%EOF di 1024 byte terakhir, blokir
 * <script/<html/<iframe/javascript:/<body. */
static bool validate_pdf_content(const std::string& data){
  if(data.size()<5) return false;
  // Must start with %PDF magic header
  if(data.rfind("%PDF",0)!=0) return false;
  // Must contain %%EOF marker in last 1024 bytes (confirms complete PDF)
  size_t tail_start = data.size()>1024 ? data.size()-1024 : 0;
  if(data.find("%%EOF",tail_start)==std::string::npos) return false;
  // Polyglot detection — reject if file contains HTML/JS signatures
  std::string lower=data;
  for(auto& c: lower) c=tolower((unsigned char)c);
  const char* bad[]={"<script","<html","<iframe","javascript:","<body","</script"};
  for(auto sig: bad) if(lower.find(sig)!=std::string::npos) return false;
  return true;
}

/* Security Gap 2: sanitasi filename — hapus path separator, strip ke
 * [a-zA-Z0-9._-], paksa ekstensi .pdf. Go reference: cleanUploadedFilename(). */
static std::string sanitize_filename(const std::string& name){
  // Ambil basename (hapus path/separator)
  auto slash=name.find_last_of("/\\");
  std::string base= slash==std::string::npos? name : name.substr(slash+1);
  std::string safe;
  for(char c: base){
    if(std::isalnum((unsigned char)c) || c=='.' || c=='-' || c=='_')
      safe.push_back(c);
  }
  if(safe.empty()) safe="exam.pdf";
  // Force .pdf extension DULU, baru truncate — supaya hasil akhir ≤128 (Bug 10)
  auto dot=safe.rfind('.');
  safe = (dot==std::string::npos) ? safe + ".pdf" : safe.substr(0,dot) + ".pdf";
  if(safe.size()>128) safe=safe.substr(0,128);
  return safe;
}
Response list_admin_exams(const Request& req){
  bool super_admin=false;
  auto it_super=req.headers.find("X-Internal-Admin-Super");
  if(it_super!=req.headers.end()) super_admin=it_super->second=="1";
  int admin_id=0;
  auto it_id=req.headers.find("X-Internal-Admin-Id");
  if(it_id!=req.headers.end()) try{ admin_id=std::stoi(it_id->second); }catch(...){ }
  auto all=exams().list_all();
  std::vector<models::Exam> snapshot;
  for(const auto& e: all){
    if(super_admin || e.created_by==admin_id || (e.delegated_to && *e.delegated_to==admin_id)) snapshot.push_back(e);
  }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::AdminExamList pb;
    pb.set_success(true);
    pb.set_total(static_cast<int32_t>(snapshot.size()));
    for(const auto& e: snapshot){
      auto *ex=pb.add_exams();
      ex->set_id(e.id); ex->set_name(e.name); ex->set_size_bytes(e.size_bytes);
      ex->set_status(e.status); ex->set_created_at(e.created_at);
    }
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  std::string json="[";
  for(size_t i=0;i<snapshot.size();++i){
    if(i) json+=",";
    const auto& e=snapshot[i];
    json+="{\"id\":"+std::to_string(e.id)
      +",\"name\":\""+json_escape(e.name)+"\""
      +",\"size_bytes\":"+std::to_string(e.size_bytes)
      +",\"status\":\""+json_escape(e.status)+"\""
      +",\"token_mode\":\""+json_escape(e.get_token_mode())+"\""
      +",\"auto_approve\":"+(e.auto_approve?"true":"false")
      +",\"tombstoned_at\":"+(e.tombstoned_at?("\""+json_escape(*e.tombstoned_at)+"\""):"null")
      +",\"created_at\":\""+json_escape(e.created_at)+"\"}";
  }
  json+="]";
  Response r; r.json(200,"{\"success\":true,\"exams\":"+json+",\"total\":"+std::to_string(snapshot.size())+"}"); return r;
}
Response create_exam(const Request& req){
  std::string idem;
  for(const auto& kv:req.headers){ std::string k=kv.first; for(char& c:k) c=tolower((unsigned char)c); if(k=="idempotency-key") { idem=kv.second; break; } }
  std::string idem_fingerprint;   // disimpan untuk finalize/release nanti
  if(!idem.empty()){
    if(idem.size()>255){ Response r; r.status=400; r.json(400,"{\"error\":\"invalid Idempotency-Key\"}"); return r; }
    idem_fingerprint = idempotency_fingerprint(req);
    auto reserve = exams().reserve_idempotency(idem, idem_fingerprint);
    if(reserve.status == store::IdempotencyStatus::Replay){
      Response r; r.status=reserve.response_status;
      if(!reserve.response_content_type.empty()) r.headers["Content-Type"]=reserve.response_content_type;
      r.body=reserve.response_body;
      return r;
    }
    if(reserve.status == store::IdempotencyStatus::Conflict){
      Response r; r.status=409; r.json(409,"{\"error\":\"Idempotency-Key conflict\",\"error_code\":\"IDEMPOTENCY_CONFLICT\"}"); return r;
    }
    // status == New → lanjut ke create flow; key sudah di-reserve.
  }
  // Helper: lepaskan idempotency reservation lalu return error response.
  // Dipakai di semua error path SETELAH reserve_idempotency() sukses.
  auto release_and_return=[&](Response r)->Response{
    if(!idem.empty()) exams().release_idempotency(idem);
    return r;
  };
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
    // Bug 2: gunakan actual size dari pdf_data, bukan client-reported size_bytes
    // (client bisa set size_bytes=1 lalu kirim 4.9MB → bypass 5MB check)
    sz = file_data.empty() && pb.size_bytes()!=0 ? std::to_string(pb.size_bytes()) : "";
    custom = pb.custom_token();
    file_data = pb.pdf_data();
    if(!file_data.empty()){
      sz = std::to_string(file_data.size());
      if(fpath.empty()) fpath = "upload.pdf";
    }
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
  if(name.empty()){ utils::log_error("exam_create_failed","reason=name_required"); Response r; r.status=400; r.json(400,"{\"error\":\"name required\"}"); return release_and_return(r); }
  // Security Gap 3: reject null bytes in name (log injection vector)
  if(has_null_bytes(name)){ Response r; r.status=400; r.json(400,"{\"error\":\"name contains invalid characters\"}"); return release_and_return(r); }
  if(name.size()>255){ Response r; r.status=400; r.json(400,"{\"error\":\"name too long\"}"); return release_and_return(r); }
  if(fpath.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"file_path required\"}"); return release_and_return(r); }
  // file_path hardening: null-byte, traversal bertingkat, backslash.
  // Skenario file_path EKSPLISIT dari form dipakai sebagai path server →
  // tolak traversal eksplisit. Skenario multipart (file_name dari filename
  // browser yang tidak bisa dipercaya) → sanitize ke basename di bawah.
  bool is_multipart_upload = !file_name.empty();
  if(has_null_bytes(fpath) || fpath.find("\\")!=std::string::npos){
    Response r; r.status=400; r.json(400,"{\"error\":\"file_path must not contain invalid characters\"}"); return release_and_return(r);
  }
  if(!is_multipart_upload && fpath.find("..")!=std::string::npos){
    Response r; r.status=400; r.json(400,"{\"error\":\"file_path must not contain traversal\"}"); return release_and_return(r);
  }
  // Security Gap 2: sanitasi file_path — hapus traversal/separator, strip ke
  // alnum/./-/_, paksa ekstensi .pdf. Dilakukan SEBELUM validasi lain supaya
  // path bersih untuk semua check berikutnya (R2 key, logging, dll).
  fpath = sanitize_filename(fpath);
  // PDF validation di SEMUA path (multipart dan protobuf) — Bug 1:
  // magic header + %%EOF marker + polyglot detection (Security Gap 1)
  if(!file_data.empty()){
    if(!validate_pdf_content(file_data)){
      Response r; r.status=400; r.json(400,"{\"error\":\"file must be valid PDF\"}"); return release_and_return(r);
    }
  }
  // size validation — sebelum R2 upload (hemat bandwidth).
  // Bug 9: cross-check terhadap ukuran file_data aktual (client bisa kirim
  // size_bytes=0/1 tapi file_data besar → pastikan tidak bypass 5MB check).
  long size=0;
  try{ if(!sz.empty()) size=std::stol(sz); else if(!file_data.empty()) size=file_data.size(); }catch(...){}
  // Bug 12: size_bytes negatif (std::stol menerima "-100") — clamp ke 0
  if(size<0) size=0;
  if(!file_data.empty()) size=std::max(size, (long)file_data.size());
  const long MAX_PDF = 5*1024*1024;
  if(size>MAX_PDF){ Response r; r.status=413; r.json(413,"{\"error\":\"file too large, max 5MB\"}"); return release_and_return(r); }
  // custom_token validasi & collision check
  // Bug 4: TIDAK pakai token_exists() di sini — periksa-ke-simpan terpisah
  // = TOCTOU race (2 thread bisa lolos). Collision di-enforce atomically
  // oleh add() (return false = token sudah dipakai) di bawah.
  std::string token;
  if(!custom.empty()){
    for(char &c: custom) c=toupper((unsigned char)c);
    if(!helpers::is_valid_exam_token(custom) || custom.size()!=8){
      Response r; r.status=400; r.json(400,"{\"error\":\"custom_token must be 8 A-Z0-9\"}"); return release_and_return(r);
    }
    // Bug A fix: claim_token secara atomik cek seen_tokens_ + exams_[],
    // bukan langsung set token. Menutup race TOCTOU bila regenerate-token
    // sedang claim token yang sama di thread lain.
    if(!exams().claim_token(custom)){
      Response r; r.status=409; r.json(409,"{\"error\":\"custom_token already in use\",\"error_code\":\"DUPLICATE_TOKEN\"}"); return release_and_return(r);
    }
    token=custom;
  } else {
    // generate unique — error eksplisit jika seluruh attempt collide
    bool found=false;
    for(int tries=0;tries<5;tries++){
      token=gen_token(8);
      if(exams().claim_token(token)){ found=true; break; }
    }
    if(!found){
      utils::log_error("exam_create_failed","reason=token_collision_exhausted");
      Response r; r.status=409; r.json(409,"{\"error\":\"token generation failed after retries\",\"error_code\":\"TOKEN_COLLISION\"}"); return release_and_return(r);
    }
  }
  // Generate ID dulu (sebelum R2 upload, supaya key pakai id nyata)
  int id = exams().next_id();
  // R2 mandatory fail-closed
  {
    auto cfg_r2 = Config::load();
    r2::R2Config rc{cfg_r2.r2_access_key, cfg_r2.r2_secret_key, cfg_r2.r2_endpoint, cfg_r2.r2_bucket};
    if(!file_data.empty() && !rc.enabled()){
      exams().unclaim_token(token); // Bug 7: lepaskan token yang sudah di-claim
      Response r; r.status=503; r.json(503,"{\"error\":\""+std::string(r2::kErrNotConfigured)+"\",\"error_code\":\""+std::string(r2::kCodeNotConfigured)+"\"}"); return release_and_return(r);
    }
    if(!file_data.empty() && rc.enabled()){
      std::string key = r2::object_key_for_exam(id, fpath);
      if(g_upload_mock){
        g_upload_mock(key, file_data);
      } else {
        r2::R2Client client{rc};
        // Upload + verifikasi object benar-benar ada di R2 (HEAD) sebelum
        // exam dianggap berhasil — ujian tanpa file = data rusak.
        bool upload_ok = client.upload(key, file_data) && client.verify(key);
        if(!upload_ok){
          // Fail-closed DEFAULT: PDF gagal diupload/diverifikasi → create BATAL (502).
          // EXAMVAN_R2_STRICT=0 adalah opt-out eksplisit (dev) agar lanjut.
          const char* strict=getenv("EXAMVAN_R2_STRICT");
          bool non_strict = strict && std::string(strict)=="0";
          if(!non_strict){
            exams().unclaim_token(token); // Bug 7: lepaskan token yang sudah di-claim
            Response r; r.status=502; r.json(502,"{\"error\":\""+std::string(r2::kErrUploadFailed)+"\",\"error_code\":\""+std::string(r2::kCodeUploadFailed)+"\"}"); return release_and_return(r);
          }
          fprintf(stderr,"[r2] upload/verify failed key=%s size=%zu, continuing (EXAMVAN_R2_STRICT=0)\n",key.c_str(),file_data.size());
        }
      }
    }
  }
  // Store via models::Exam (lengkap, bukan triplet)
  // Bug 4: gunakan return value add() sebagai atomic collision guard
  // (token_exists + add terpisah = TOCTOU race → 2 exam token sama)
  models::Exam exam;
  exam.id=id;
  exam.name=name;
  exam.file_path=fpath;
  exam.size_bytes=size;
  exam.token=token;
  exam.active_token=token; // Go parity: active_token = token saat create
  exam.status="inactive";
  // created_by dari session admin (diinjeksi admin_api dari cookie terverifikasi).
  // Tanpa ini INSERT gagal FK exams_created_by_fkey → admin_users(id) di schema
  // Go (sebelumnya selalu 0 → create_exam tak pernah berhasil di DB nyata).
  const std::string kInternalAdminIdHeader = "X-Internal-Admin-Id";
  for(const auto& kv:req.headers){
    std::string k=kv.first; for(char& c:k) c=tolower((unsigned char)c);
    std::string want=kInternalAdminIdHeader; for(char& c:want) c=tolower((unsigned char)c);
    if(k==want){
      try{ exam.created_by=std::stoi(kv.second); }catch(...){}
      break;
    }
  }
  exam.security_level="medium";
  exam.created_at=helpers::format_iso_utc(std::chrono::system_clock::now());
  if(!exams().add(exam)){
    exams().unclaim_token(token); // cleanup
    // R2 orphan cleanup: upload DB insert gagal → hapus object yang sudah di-upload.
    if(!file_data.empty()){
      auto cfg_r2 = Config::load();
      r2::R2Config rc{cfg_r2.r2_access_key, cfg_r2.r2_secret_key, cfg_r2.r2_endpoint, cfg_r2.r2_bucket};
      if(rc.enabled()){
        std::string key = r2::object_key_for_exam(id, fpath);
        r2::R2Client client{rc};
        client.remove(key);
      } else if(g_upload_mock){
        std::string key = r2::object_key_for_exam(id, fpath);
        g_upload_mock(key, "");  // empty data → mock cleanup marker (test hook)
      }
    }
    Response r; r.status=409; r.json(409,"{\"error\":\"custom_token already in use\",\"error_code\":\"DUPLICATE_TOKEN\"}"); return release_and_return(r);
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
  Response r; r.status=201; r.json(201,"{\"success\":true,\"id\":"+std::to_string(id)+",\"token\":\""+esc_token+"\",\"name\":\""+esc_name+"\",\"file_path\":\""+esc_fpath+"\",\"status\":\"inactive\",\"size_bytes\":"+std::to_string(size)+",\"created_at\":\""+exam.created_at+"\",\"message\":\"Ujian berhasil diunggah\"}");
  if(!idem.empty()){
    // Durable finalization: simpan response representation untuk replay lintas
    // proses/restart. PostgreSQL versi atomik via transaksi (exam INSERT terlibat
    // di finalize_idempotency() store; di sini add() sudah berhasil sebelum finalize,
    // jadi finalize cukup menyimpan response + state completed).
    exams().finalize_idempotency(idem, idem_fingerprint, r.status, r.body,
                                 r.headers.count("Content-Type")? r.headers.at("Content-Type") : "");
  }
  return r;
}
static std::string get_exam_id(const Request& req){
  auto it=req.params.find("id");
  if(it!=req.params.end()) return it->second;
  it=req.params.find("exam_id");
  if(it!=req.params.end()) return it->second;
  return "";
}

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

static std::optional<int> json_int_field(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t p=body.find(needle);
  if(p==std::string::npos) return std::nullopt;
  size_t colon=body.find(':',p+needle.size());
  if(colon==std::string::npos) return std::nullopt;
  size_t s=body.find_first_not_of(" \t\r\n",colon+1);
  if(s==std::string::npos) return std::nullopt;
  try { size_t n=0; int value=std::stoi(body.substr(s),&n); (void)n; return value; }
  catch(...) { return std::nullopt; }
}

// Parse boolean JSON (true/false/1/0) — json_int_field (stoi) GAGAL untuk
// true/false, padahal frontend mengirim strict_mode sebagai boolean.
static std::optional<bool> json_bool_field(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t p=body.find(needle);
  if(p==std::string::npos) return std::nullopt;
  size_t colon=body.find(':',p+needle.size());
  if(colon==std::string::npos) return std::nullopt;
  size_t s=body.find_first_not_of(" \t\r\n",colon+1);
  if(s==std::string::npos) return std::nullopt;
  if(body.compare(s,4,"true")==0 || (body[s]=='1')) return true;
  if(body.compare(s,5,"false")==0 || body[s]=='0') return false;
  return std::nullopt;
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
    if(action.empty()){ p=req.path.find("/edit-token"); if(p!=std::string::npos) action="edit-token"; }
    if(action.empty()){ p=req.path.find("/token-mode"); if(p!=std::string::npos) action="token-mode"; }
    if(action.empty()){ p=req.path.find("/edit"); if(p!=std::string::npos) action="edit"; }
  }
  // handle actions yang memerlukan body JSON: edit-token, token-mode
  if(action=="edit-token" || action=="token-mode"){
    std::string raw_token;
    std::optional<int> raw_interval;
    std::string raw_mode;
    if(req.headers.count("Content-Type") &&
       req.headers.at("Content-Type").find("application/json")!=std::string::npos){
      raw_token=json_string_field(req.body,"token");
      raw_interval=json_int_field(req.body,"reset_interval");
      raw_mode=json_string_field(req.body,"token_mode");
    } else {
      auto form=helpers::parse_form(req.body);
      if(form.count("token")) raw_token=form["token"];
      if(form.count("reset_interval")) { try{ raw_interval=std::stoi(form["reset_interval"]); }catch(...){} }
      if(form.count("token_mode")) raw_mode=form["token_mode"];
    }
    if(action=="edit-token"){
      std::string new_tok=raw_token;
      // trim + uppercase (Go parity: tokenRegex ^[A-Z0-9]{8}$)
      std::string t;
      size_t b=new_tok.find_first_not_of(" \t\r\n");
      size_t e=new_tok.find_last_not_of(" \t\r\n");
      if(b==std::string::npos) t="";
      else t=new_tok.substr(b,e-b+1);
      for(char &ch: t) ch=toupper((unsigned char)ch);
      if(t.empty()){
        Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"token not allowed to be empty\"}"); return r;
      }
      if(!helpers::is_valid_exam_token(t) || t.size()!=8){
        Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"token must be 8 A-Z0-9\"}"); return r;
      }
      // cek collision terhadap exam lain
      if(exams().token_exists(t, id)){
        Response r; r.status=409; r.json(409,"{\"success\":false,\"error\":\"token already in use\",\"error_code\":\"DUPLICATE_TOKEN\"}"); return r;
      }
      std::string old_token;
      bool updated=exams().update(id,[&](models::Exam& e){ old_token=e.token; e.token=t; e.active_token=t; e.token_last_reset_at=helpers::format_iso_utc(std::chrono::system_clock::now()); });
      if(!updated){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
      if(!old_token.empty()) exams().unclaim_token(old_token);
      if(!exams().claim_token(t)){ /* sudah ada — jangan gagalkan (sudah disimpan) */ }
      Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+",\"token\":\""+json_escape(t)+"\",\"message\":\"Token ujian berhasil diubah\"}"); return r;
    }
    if(action=="token-mode"){
      std::string mode=raw_mode;
      for(char &ch: mode) ch=tolower((unsigned char)ch);
      if(mode!="static" && mode!="dynamic"){
        Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid token mode\"}"); return r;
      }
      if(mode=="dynamic"){
        if(!raw_interval.has_value() || *raw_interval < 1){
          Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"reset_interval required (minimal 1 menit)\"}"); return r;
        }
      }
      bool updated=exams().update(id,[&](models::Exam& e){
        e.token_mode=mode;
        if(mode=="static"){ e.token_reset_interval.reset(); e.token_last_reset_at.reset(); }
        else { e.token_reset_interval=*raw_interval; }
      });
      if(!updated){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
      Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+",\"token_mode\":\""+json_escape(mode)+"\",\"message\":\"Mode token berhasil diperbarui\"}"); return r;
    }
  }
  // regenerate-token: claim token baru (atomic thd store) lalu terapkan via update()
  if(action=="regenerate-token"){
    std::string new_token;
    bool ok=false;
    for(int tries=0;tries<5;tries++){
      new_token=gen_token(8);
      if(exams().claim_token(new_token)){ ok=true; break; }
    }
    if(ok){
      std::string old_token;
      bool found = exams().update(id, [&](models::Exam& e){ old_token=e.token; e.token=new_token; e.active_token=new_token; });
      if(!found){
        exams().unclaim_token(new_token); // cleanup — exam tidak ditemukan
        Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r;
      }
      if(!old_token.empty()) exams().unclaim_token(old_token); // Bug 1: lepas token lama dari seen_tokens_
      utils::log_info("exam_token_regenerated","id="+id_str);
      Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+",\"token\":\""+json_escape(new_token)+"\",\"message\":\"Token berhasil dibuat ulang\"}"); return r;
    } else {
      // Bug 8: claim selalu gagal → return 500, bukan 200 dengan token lama
      utils::log_error("exam_token_regenerate_failed","id="+id_str+" reason=token_collision_exhausted");
      Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"token generation failed after retries\",\"error_code\":\"TOKEN_COLLISION\"}"); return r;
    }
  }
  // semua action lain: mutasi via store.update() (mutator jalan dalam lock store)
  // Bug E: reject action tidak dikenal SEBELUM acquire lock store
  if(action!="toggle" && action!="start" && action!="stop" && action!="edit" && action!="edit-token" && action!="token-mode"){
    Response r; r.status=400; r.json(400,"{\"error\":\"unknown action: "+json_escape(action)+"\"}"); return r;
  }
  // pre-validasi nama hanya untuk action edit
  std::string new_name;
  // submitEditExam (frontend) mengirim FormData multipart: name + pdf_file
  // (opsional). Sebelumnya pdf_file DIABAIKAN → ganti PDF di modal edit ujian
  // diam-diam tidak terjadi.
  std::string edit_pdf_name, edit_pdf_data, edit_pdf_ct;
  if(action=="edit"){
    bool is_multipart=false;
    std::string ct_hdr;
    for(auto& kv:req.headers){ std::string k=kv.first; for(char& ch:k) ch=tolower((unsigned char)ch); if(k=="content-type"){ ct_hdr=kv.second; break; } }
    if(ct_hdr.find("multipart/form-data")!=std::string::npos) is_multipart=true;
    if(is_multipart){
      std::map<std::string,std::string> form;
      parse_multipart(req.body, ct_hdr, form, edit_pdf_name, edit_pdf_data, edit_pdf_ct);
      auto fn=form.find("name"); if(fn!=form.end()) new_name=fn->second;
    } else {
      new_name=req.params.count("name")? req.params.at("name") : "";
      if(new_name.empty()){
        auto form=helpers::parse_form(req.body);
        new_name = form.count("name")? form["name"] : "";
      }
    }
    // Bug 3: edit wajib ada param name — error spesifik, bukan generik
    if(new_name.empty()){
      Response r; r.status=400; r.json(400,"{\"error\":\"edit requires name field\"}"); return r;
    }
    new_name=helpers::sanitize_student_input(new_name);
    if(new_name.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"name required\"}"); return r; }
    if(has_null_bytes(new_name)){ Response r; r.status=400; r.json(400,"{\"error\":\"name contains invalid characters\"}"); return r; }
    if(new_name.size()>255){ Response r; r.status=400; r.json(400,"{\"error\":\"name too long\"}"); return r; }
    // PDF baru (opsional): validasi + upload R2 + verifikasi (mirror create_exam).
    if(!edit_pdf_data.empty()){
      if(!validate_pdf_content(edit_pdf_data)){
        Response r; r.status=400; r.json(400,"{\"error\":\"file must be valid PDF\"}"); return r;
      }
      if(edit_pdf_data.size()>5*1024*1024){
        Response r; r.status=413; r.json(413,"{\"error\":\"file too large, max 5MB\"}"); return r;
      }
      // Treat multipart filenames as untrusted browser metadata, exactly like
      // create uploads: normalize basename, allowlist chars, force .pdf.
      edit_pdf_name=sanitize_filename(edit_pdf_name);
      if(edit_pdf_name.empty()){
        Response r; r.status=400; r.json(400,"{\"error\":\"invalid PDF filename\"}"); return r;
      }
      auto cfg_r2=Config::load();
      r2::R2Config rc{cfg_r2.r2_access_key, cfg_r2.r2_secret_key, cfg_r2.r2_endpoint, cfg_r2.r2_bucket};
      if(!rc.enabled()){
        Response r; r.status=503; r.json(503,"{\"error\":\""+std::string(r2::kErrNotConfigured)+"\",\"error_code\":\""+std::string(r2::kCodeNotConfigured)+"\"}"); return r;
      }
      std::string key=r2::object_key_for_exam(id, edit_pdf_name);
      if(g_upload_mock){
        g_upload_mock(key, edit_pdf_data);
      } else {
        r2::R2Client client{rc};
        if(!(client.upload(key, edit_pdf_data) && client.verify(key))){
          const char* strict=getenv("EXAMVAN_R2_STRICT");
          bool non_strict=strict && std::string(strict)=="0";
          if(!non_strict){
            Response r; r.status=502; r.json(502,"{\"error\":\""+std::string(r2::kErrUploadFailed)+"\",\"error_code\":\""+std::string(r2::kCodeUploadFailed)+"\"}"); return r;
          }
        }
      }
    }
  }
  std::string result_status;
  std::string result_name;
  bool found=false;
  bool already_started=false;
  found = exams().update(id, [&](models::Exam& e){
    if(action=="toggle"){
      const bool activate = !e.is_active();
      e.status = activate ? "active" : "inactive";
      // Go parity: re-activation removes an automatic tombstone. Toggle
      // deliberately preserves exam_started_at, active_token and reset clock.
      if(activate) e.tombstoned_at.reset();
      result_status=e.status;
    }
    else if(action=="start"){
      // Check and mutation happen under the same store lock. This mirrors
      // Go's already-started guard without a check-then-update race.
      if(e.exam_started_at.has_value() && !e.exam_started_at->empty()){
        already_started=true;
        return;
      }
      const auto now=helpers::format_iso_utc(std::chrono::system_clock::now());
      e.status="active";
      e.exam_started_at=now;
      e.token_last_reset_at=now;
      e.tombstoned_at.reset();
      e.active_token=e.token; // Go parity: permanent token is initial active token
      result_status="active";
    }
    else if(action=="stop"){
      e.status="inactive";
      e.exam_started_at.reset(); // Go parity: stopping clears the started marker
      result_status="inactive";
    }
    else if(action=="edit"){
      if(!new_name.empty()){ e.name=new_name; result_name=new_name; }
      if(!edit_pdf_data.empty()){ e.file_path=edit_pdf_name; e.size_bytes=edit_pdf_data.size(); }
    }
  });
  if(!found){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
  if(already_started){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Ujian sudah dimulai\"}"); return r;
  }
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
    Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+",\"status\":\""+result_status+"\",\"new_status\":\""+result_status+"\",\"message\":\"Status diperbarui\"}"); return r;
  }
  if(!result_name.empty()){
    utils::log_info("exam_updated","id="+id_str+" action=edit name="+result_name);
    Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+",\"name\":\""+json_escape(result_name)+"\",\"message\":\"Nama diperbarui\"}"); return r;
  }
  // tidak ada action dikenal / tidak ada perubahan -> 400
  Response r; r.status=400; r.json(400,"{\"error\":\"no valid update field\"}"); return r;
}

Response delete_exam(const Request& req){
  auto id_str=get_exam_id(req);
  if(id_str.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"exam id required\"}"); return r; }
  int id=0;
  try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"error\":\"invalid exam id\"}"); return r; }
  auto exam=exams().get_by_id(id);
  if(!exam){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
  // K3: hapus object PDF di R2 juga — jangan tinggalkan orphan (frontend
  // menjanjikan "File PDF juga akan dihapus permanen").
  auto cfg_r2=Config::load();
  r2::R2Config rc{cfg_r2.r2_access_key, cfg_r2.r2_secret_key, cfg_r2.r2_endpoint, cfg_r2.r2_bucket};
  // C6: hapus kedua layout — pdfs/{file_path} (era Go) dan exams/{id}/... (C++).
  std::vector<std::string> keys={r2::object_key_pdf_legacy(exam->file_path),
                                 r2::object_key_for_exam(id, exam->file_path)};
  if(g_upload_mock){
    for(auto& k: keys) g_upload_mock(k, ""); // data kosong = penanda penghapusan (test hook)
  } else if(rc.enabled()){
    r2::R2Client client{rc};
    for(auto& key: keys){
      if(!client.remove(key)){
        utils::log_error("exam_delete_r2_failed","id="+id_str+" key="+key);
      }
    }
  } else {
    // Go parity: tanpa R2, PDF tidak bisa dibersihkan → tolak delete agar
    // tidak ada baris DB tanpa object R2. Frontend menampilkan warning.
    Response r; r.status=503; r.json(503,"{\"success\":false,\"error\":\""+std::string(r2::kErrNotConfigured)+"\",\"error_code\":\""+std::string(r2::kCodeNotConfigured)+"\"}"); return r;
  }
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
  Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+",\"message\":\"Ujian dihapus\"}"); return r;
}
Response export_xlsx(const Request& req){
  // Export per-ujian: delegasi ke export_submissions_xlsx (export.cpp) yang
  // membaca :id dari params dan membangun XLSX nyata (zip + XML valid).
  return export_submissions_xlsx(req);
}
// ---- Konversi jadwal WIB → UTC ISO (paritas Go SaveQuestions) ----
// Go menerima "YYYY-MM-DD HH:MM" (Asia/Jakarta, UTC+7 tanpa DST) lalu
// menyimpan UTC ISO "YYYY-MM-DDTHH:MM:SSZ". C++ sebelumnya menyimpan
// mentah format lokal → jadwal tidak kompatibel dengan exam buatan Go.
static int days_from_civil(int y, unsigned m, unsigned d){
  y -= (int)(m <= 2);
  const int era = (y >= 0 ? y : y-399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u*(m + (m > 2 ? -3 : 9)) + 2u)/5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe/4u - yoe/100u + doy;
  return era * 146097 + (int)doe - 719468;
}
static void civil_from_days(int z, int& y, unsigned& m, unsigned& d){
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe/1460u + doe/36524u - doe/146096u) / 365u;
  const int y2 = (int)yoe + era * 400;
  const unsigned doy = doe - (365u*yoe + yoe/4u - yoe/100u);
  const unsigned mp = (5u*doy + 2u)/153u;
  const unsigned d2 = doy - (153u*mp + 2u)/5u + 1u;
  const unsigned m2 = mp < 10u ? mp + 3u : mp - 9u;
  y = y2 + (int)(m2 <= 2u); m = m2; d = d2;
}
// "YYYY-MM-DD HH:MM" (WIB) → "YYYY-MM-DDTHH:MM:SSZ" (UTC). nullopt bila format invalid.
static std::optional<std::string> wib_to_utc_iso(const std::string& s){
  if(s.size()<16) return std::nullopt;
  int y=0,mo=0,d=0,h=0,mi=0;
  if(std::sscanf(s.c_str(),"%d-%d-%d %d:%d",&y,&mo,&d,&h,&mi)!=5) return std::nullopt;
  if(y<2000||y>2100||mo<1||mo>12||d<1||d>31||h<0||h>23||mi<0||mi>59) return std::nullopt;
  long long total=days_from_civil(y,(unsigned)mo,(unsigned)d)*1440LL + h*60LL + mi - 7LL*60;
  long long days=total/1440; long long rem=total%1440;
  if(rem<0){ rem+=1440; days-=1; }
  int y2; unsigned m2,d2;
  civil_from_days((int)days,y2,m2,d2);
  int hh=(int)(rem/60), mm=(int)(rem%60);
  char buf[64];
  snprintf(buf,sizeof(buf),"%04d-%02u-%02uT%02d:%02d:00Z",y2,m2,d2,hh,mm);
  return std::string(buf);
}

// Validasi struktur array soal: tipe whitelist + field wajib per tipe.
// Go tidak memvalidasi, tetapi frontend hanya bisa menghasilkan struktur ini
// — menolak lebih awal mencegah soal rusak tersimpan lalu membuat scoring
// pekerja / app Android gagal saat ujian berlangsung.
static bool validate_questions_array(const std::string& raw){
  if(raw.empty()) return true;
  if(raw.front()!='[') return false;
  std::vector<std::string> objs;
  size_t i=0;
  while(i<raw.size()){
    while(i<raw.size() && (raw[i]==' '||raw[i]=='\t'||raw[i]=='\r'||raw[i]=='\n'||raw[i]==','||raw[i]=='['||raw[i]==']')) i++;
    if(i>=raw.size()) break;
    if(raw[i]=='{'){
      int depth=0; size_t s=i; bool in=false,esc=false;
      for(;i<raw.size();++i){
        char c=raw[i];
        if(esc){esc=false;continue;}
        if(c=='\\'&&in){esc=true;continue;}
        if(c=='"'){in=!in;continue;}
        if(in) continue;
        if(c=='{')depth++;
        else if(c=='}'){depth--; if(depth==0){i++; objs.push_back(raw.substr(s,i-s)); break;}}
      }
    } else return false;
  }
  if(objs.empty()) return true; // [] valid
  static const char* kTypes[]={"single_choice","multiple_choice","true_false","matching","short_answer"};
  for(auto& o: objs){
    std::string type=json_string_field(o,"type");
    bool known=false;
    for(auto* t:kTypes){ if(type==t){known=true;break;} }
    if(!known) return false;
    if(!json_int_field(o,"number").has_value()) return false;
    std::string key=json_raw_value(o,"key");
    std::string choices=json_raw_value(o,"choices");
    std::string left=json_raw_value(o,"left_items");
    std::string right=json_raw_value(o,"right_items");
    if(type=="matching"){
      if(key.empty()||key.front()!='{') return false;
      if(left.empty()||left=="[]"||right.empty()||right=="[]") return false;
    } else if(type=="multiple_choice"){
      if(key.empty()||key.front()!='['||key=="[]") return false;
      if(choices.empty()||choices=="[]") return false;
    } else if(type=="single_choice"||type=="true_false"){
      if(json_string_field(o,"key").empty()) return false;
      if(type=="single_choice" && (choices.empty()||choices=="[]")) return false;
    }
    // short_answer: key boleh kosong (frontend menghasilkan '')
  }
  return true;
}

// Parse JSON array angka, mis. pengawas_ids: [3,7] → {3,7}. Return false
// bila elemen non-numerik ditemukan (validasi).
static bool parse_int_array(const std::string& raw, std::vector<int>& out){
  out.clear();
  size_t i=0;
  while(i<raw.size() && (raw[i]==' '||raw[i]=='\t'||raw[i]=='\r'||raw[i]=='\n')) i++;
  if(i>=raw.size()||raw[i]!='[') return false;
  i++;
  while(i<raw.size()){
    while(i<raw.size() && (raw[i]==' '||raw[i]=='\t'||raw[i]=='\r'||raw[i]=='\n'||raw[i]==',')) i++;
    if(i>=raw.size()||raw[i]==']') break;
    if(!isdigit((unsigned char)raw[i])) return false;
    size_t s=i; while(i<raw.size() && isdigit((unsigned char)raw[i])) i++;
    try{ out.push_back(std::stoi(raw.substr(s,i-s))); }catch(...){ return false; }
  }
  return true;
}

// List pengawas (assigned per exam / available) dari PG — paritas Go
// exam_pengawas junction + admin_users.role ILIKE '%"pengawas"%'.
static std::string query_pengawas_json(int exam_id, bool assigned_only){
  std::string result="[]";
#ifdef HAS_LIBPQ
  try{
    auto cfg_db=Config::load();
    examvan::DbPool pool(cfg_db.database_url, 10);
    examvan::db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url), 10);
    if(auto c=real.acquire()){
      const char* sql=assigned_only
        ? "SELECT ep.user_id,u.username,COALESCE(u.name,''),COALESCE(u.instansi,'') FROM exam_pengawas ep JOIN admin_users u ON ep.user_id=u.id WHERE ep.exam_id=$1 ORDER BY u.username"
        : "SELECT id,username,COALESCE(name,''),COALESCE(instansi,'') FROM admin_users WHERE role ILIKE '%\"pengawas\"%' AND status='active' ORDER BY username";
      auto r=real.exec_params(c.get(),sql,assigned_only?std::vector<std::string>{std::to_string(exam_id)}:std::vector<std::string>{});
      if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
        std::string arr="[";
        for(int i=0;i<PQntuples(r.get());i++){
          if(i>0) arr+=",";
          arr+="{\"id\":"+std::string(PQgetvalue(r.get(),i,0))
            +",\"username\":\""+json_escape(PQgetvalue(r.get(),i,1))+"\""
            +",\"name\":\""+json_escape(PQgetvalue(r.get(),i,2))+"\""
            +",\"instansi\":\""+json_escape(PQgetvalue(r.get(),i,3))+"\"}";
        }
        arr+="]";
        result=arr;
      }
      real.release(c.release());
    }
  }catch(...){ /* best-effort: tanpa PG, [] (paritas perilaku sebelum-sebelumnya) */ }
#endif
  return result;
}

Response get_exam_questions(const Request& req){
  auto id_str=get_exam_id(req);
  if(id_str.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"exam id required\"}"); return r; }
  int id=0;
  try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"error\":\"invalid exam id\"}"); return r; }
  auto exam=exams().get_by_id(id);
  if(!exam){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
  // questions/identity_fields tersimpan sebagai JSON mentah (TEXT); [] bila kosong.
  std::string questions = exam->questions_json.has_value() && !exam->questions_json->empty()
    ? *exam->questions_json : "[]";
  std::string identity = exam->identity_fields.has_value() && !exam->identity_fields->empty()
    ? *exam->identity_fields : "[]";
  std::string json="{\"success\":true,\"id\":"+std::to_string(id)
    +",\"token\":\""+json_escape(exam->token)+"\""
    +",\"security_level\":\""+json_escape(exam->security_level)+"\""
    +",\"strict_mode\":"+(exam->strict_mode?"true":"false")
    +",\"public_results\":"+std::to_string(exam->public_results)
    +",\"show_answers\":"+std::to_string(exam->show_answers)
    +",\"panel_color\":"+(exam->panel_color?("\""+json_escape(*exam->panel_color)+"\""):"null")
    +",\"start_time\":"+(exam->start_time?("\""+json_escape(*exam->start_time)+"\""):"null")
    +",\"end_time\":"+(exam->end_time?("\""+json_escape(*exam->end_time)+"\""):"null")
    +",\"congrats_message\":"+(exam->congrats_message?("\""+json_escape(*exam->congrats_message)+"\""):"null")
    +",\"questions\":"+questions
    +",\"identity_fields\":"+identity
    +",\"assigned_pengawas\":"+query_pengawas_json(id,true)
    +",\"available_pengawas\":"+query_pengawas_json(id,false)+"}";
  Response r; r.json(200, json); return r;
}
Response save_exam_questions(const Request& req){
  auto id_str=get_exam_id(req);
  if(id_str.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"exam id required\"}"); return r; }
  int id=0;
  try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"error\":\"invalid exam id\"}"); return r; }
  if(!exams().get_by_id(id)){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
  std::string questions=json_raw_value(req.body,"questions");
  std::string identity=json_raw_value(req.body,"identity_fields");
  std::string sec=json_string_field(req.body,"security_level");
  std::string color=json_string_field(req.body,"panel_color");
  std::string st=json_string_field(req.body,"start_time");
  std::string et=json_string_field(req.body,"end_time");
  std::string congrats=json_string_field(req.body,"congrats_message");
  // strict_mode dikirim frontend sebagai BOOLEAN (true/false) — json_int_field
  // (stoi) tidak bisa parse → gunakan json_bool_field.
  auto strict=json_bool_field(req.body,"strict_mode");
  // Pengawas assignment (paritas Go exam_pengawas junction).
  std::string pengawas_raw=json_raw_value(req.body,"pengawas_ids");
  std::vector<int> pengawas_ids;
  if(!pengawas_raw.empty() && !parse_int_array(pengawas_raw,pengawas_ids)){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"pengawas_ids must be an array of numbers\"}"); return r;
  }
  // Validasi: questions/identity_fields wajib array bila dikirim (bukan string).
  if(!questions.empty() && questions.front()!='['){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"questions must be an array\"}"); return r;
  }
  if(!identity.empty() && identity.front()!='['){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"identity_fields must be an array\"}"); return r;
  }
  if(!sec.empty() && sec!="low" && sec!="medium" && sec!="high"){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid security_level\"}"); return r;
  }
  // Validasi struktur tiap soal (tipe whitelist + field wajib per tipe).
  if(!questions.empty() && !validate_questions_array(questions)){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"questions structure invalid\"}"); return r;
  }
  // Jadwal: input WIB "YYYY-MM-DD HH:MM" → simpan UTC ISO (paritas Go).
  // Format selain itu ditolak 400 — jangan telan input tak valid diam-diam.
  std::optional<std::string> st_iso=wib_to_utc_iso(st);
  std::optional<std::string> et_iso=wib_to_utc_iso(et);
  if(!st.empty() && !st_iso){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Format jadwal mulai tidak valid — gunakan format: YYYY-MM-DD HH:MM\"}"); return r;
  }
  if(!et.empty() && !et_iso){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Format jadwal selesai tidak valid — gunakan format: YYYY-MM-DD HH:MM\"}"); return r;
  }
  // Panel color (paritas Go): wajib diawali #, maks 7 karakter.
  if(!color.empty() && color[0]!='#') color="";
  if(color.size()>7) color=color.substr(0,7);
  // strict_mode diturunkan server-side dari security_level (paritas Go):
  // high → 1, selain itu → 0. Nilai klien diabaikan bila level dikirim.
  int strict_derived=0;
  if(!sec.empty()) strict_derived=(sec=="high")?1:0;
  bool updated=exams().update(id,[&](models::Exam& e){
    if(!questions.empty()) e.questions_json=questions;
    if(!identity.empty()) e.identity_fields=identity;
    if(!sec.empty()) e.security_level=sec;
    if(!sec.empty()) e.strict_mode=strict_derived;
    else if(strict.has_value()) e.strict_mode=*strict;
    if(!color.empty()) e.panel_color=color; else e.panel_color.reset();
    if(!st.empty() && st_iso) e.start_time=*st_iso; else e.start_time.reset();
    if(!et.empty() && et_iso) e.end_time=*et_iso; else e.end_time.reset();
    if(!congrats.empty()) e.congrats_message=congrats; else e.congrats_message.reset();
  });
  if(!updated){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
  // Simpan assignment pengawas: replace semua baris exam_pengawas dalam satu
  // transaksi (paritas Go). Best-effort: gagal PG tidak menggagalkan simpan soal.
  if(!pengawas_raw.empty()){
#ifdef HAS_LIBPQ
    try{
      auto cfg_db=Config::load();
      examvan::DbPool pool(cfg_db.database_url, 10);
      examvan::db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url), 10);
      if(auto c=real.acquire()){
        real.exec_params(c.get(),"BEGIN",{});
        real.exec_params(c.get(),"DELETE FROM exam_pengawas WHERE exam_id=$1",{std::to_string(id)});
        for(int uid: pengawas_ids){
          real.exec_params(c.get(),"INSERT INTO exam_pengawas (exam_id,user_id) VALUES ($1,$2)",{std::to_string(id),std::to_string(uid)});
        }
        real.exec_params(c.get(),"COMMIT",{});
        real.release(c.release());
      }
    }catch(...){ utils::log_error("exam_pengawas_save_failed","id="+id_str); }
#endif
  }
  utils::log_info("exam_questions_saved","id="+id_str);
  Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Konfigurasi soal berhasil disimpan\"}"); return r;
}

// ===== Bulk toggle ==========================================================
// POST /admin/api/exams/bulk-toggle {ids:[...], status:"active"|"inactive"}
// (frontend bulkToggleExams). Sebelumnya route tidak ada → 404.
Response bulk_toggle_exams(const Request& req){
  std::string ids_raw=json_raw_value(req.body,"ids");
  std::string status=json_string_field(req.body,"status");
  std::vector<int> ids;
  if(ids_raw.empty() || !parse_int_array(ids_raw,ids) || ids.empty()){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"ids must be a non-empty array of numbers\"}"); return r;
  }
  if(status!="active" && status!="inactive"){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"status must be active or inactive\"}"); return r;
  }
  int ok_count=0;
  for(int id: ids){
    bool found=exams().update(id,[&](models::Exam& e){
      e.status=status;
      if(status=="active") e.tombstoned_at.reset(); // Go parity: re-activation clears tombstone
    });
    if(found) ok_count++;
  }
  utils::log_info("exams_bulk_toggled","count="+std::to_string(ok_count)+" status="+status);
  Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"updated\":"+std::to_string(ok_count)+",\"message\":\"Status "+std::to_string(ok_count)+" ujian berhasil diperbarui\"}"); return r;
}

// ===== Bulk delete ==========================================================
// POST /admin/api/exams/bulk-delete {ids:[...]} (frontend bulkDeleteExams).
// R2 cleanup per exam (mirror delete_exam) lalu hapus dari store.
Response bulk_delete_exams(const Request& req){
  std::string ids_raw=json_raw_value(req.body,"ids");
  std::vector<int> ids;
  if(ids_raw.empty() || !parse_int_array(ids_raw,ids) || ids.empty()){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"ids must be a non-empty array of numbers\"}"); return r;
  }
  int ok_count=0;
  for(int id: ids){
    auto exam=exams().get_by_id(id);
    if(!exam) continue;
    auto cfg_r2=Config::load();
    r2::R2Config rc{cfg_r2.r2_access_key, cfg_r2.r2_secret_key, cfg_r2.r2_endpoint, cfg_r2.r2_bucket};
    std::string key=r2::object_key_for_exam(id, exam->file_path);
    if(g_upload_mock){
      g_upload_mock(key, "");
    } else if(rc.enabled()){
      r2::R2Client client{rc};
      if(!client.remove(key)) utils::log_error("exam_bulk_delete_r2_failed","id="+std::to_string(id));
    } else {
      // Tanpa R2 object tidak bisa dibersihkan → lewati ujian ini (paritas delete_exam).
      continue;
    }
    if(exams().remove(id)) ok_count++;
  }
  utils::log_info("exams_bulk_deleted","count="+std::to_string(ok_count));
  Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"deleted\":"+std::to_string(ok_count)+",\"message\":\""+std::to_string(ok_count)+" ujian dihapus\"}"); return r;
}

// ===== Delegate exam =========================================================
// GET /admin/api/exams/:exam_id/delegate-data + POST .../delegate
// (modal delegasi operator). Sebelumnya route tidak ada → modal 404.
//
// Tanpa PG (HAS_LIBPQ=0): kembalikan bentuk kosong agar modal tetap terbuka.
// Dengan PG: list guru/pengawas satu instansi (paritas Go DelegateData).
#ifdef HAS_LIBPQ
static int session_admin_id_from(const Request& req){
  for(auto& kv:req.headers){ std::string k=kv.first; for(char& ch:k) ch=tolower((unsigned char)ch); if(k=="x-internal-admin-id"){ try{ return std::stoi(kv.second); }catch(...){} } }
  return 0;
}
#endif

Response delegate_data(const Request& req){
  auto id_str=get_exam_id(req);
  if(id_str.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"exam id required\"}"); return r; }
  int id=0;
  try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"error\":\"invalid exam id\"}"); return r; }
  auto exam=exams().get_by_id(id);
  if(!exam){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
  std::string current_owner="null", delegated_to="null", gurus="[]", pengawas="[]", assigned="[]";
#ifdef HAS_LIBPQ
  int uid=session_admin_id_from(req);
  try{
    auto cfg_db=Config::load();
    examvan::DbPool pool(cfg_db.database_url, 10);
    examvan::db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url), 10);
    if(auto c=real.acquire()){
      auto ui=real.exec_params(c.get(),"SELECT instansi FROM admin_users WHERE id=$1",{std::to_string(uid)});
      if(ui && PQresultStatus(ui.get())==PGRES_TUPLES_OK && PQntuples(ui.get())>0){
        std::string inst=PQgetvalue(ui.get(),0,0);
        // current owner (created_by)
        auto ow=real.exec_params(c.get(),"SELECT id,username FROM admin_users WHERE id=$1",{std::to_string(exam->created_by)});
        if(ow && PQresultStatus(ow.get())==PGRES_TUPLES_OK && PQntuples(ow.get())>0){
          current_owner="{\"id\":"+std::string(PQgetvalue(ow.get(),0,0))+",\"username\":\""+json_escape(PQgetvalue(ow.get(),0,1))+"\"}";
        }
        // delegated_to
        if(exam->delegated_to.has_value()){
          auto dt=real.exec_params(c.get(),"SELECT id,username FROM admin_users WHERE id=$1",{std::to_string(*exam->delegated_to)});
          if(dt && PQresultStatus(dt.get())==PGRES_TUPLES_OK && PQntuples(dt.get())>0){
            delegated_to="{\"id\":"+std::string(PQgetvalue(dt.get(),0,0))+",\"username\":\""+json_escape(PQgetvalue(dt.get(),0,1))+"\"}";
          }
        }
        // available gurus: instansi sama, active, role guru, exclude creator
        auto gr=real.exec_params(c.get(),
          "SELECT id,username,COALESCE(instansi,'') FROM admin_users WHERE instansi=$1 AND status='active' AND role ILIKE '%\"guru\"%' AND id<>$2 ORDER BY username",
          {inst,std::to_string(exam->created_by)});
        if(gr && PQresultStatus(gr.get())==PGRES_TUPLES_OK){
          std::string arr="[";
          for(int i=0;i<PQntuples(gr.get());i++){
            if(i>0) arr+=",";
            arr+="{\"id\":"+std::string(PQgetvalue(gr.get(),i,0))+",\"username\":\""+json_escape(PQgetvalue(gr.get(),i,1))+"\",\"instansi\":\""+json_escape(PQgetvalue(gr.get(),i,2))+"\"}";
          }
          arr+="]"; gurus=arr;
        }
        // available pengawas: instansi sama, active, role pengawas
        auto pw=real.exec_params(c.get(),
          "SELECT id,username,COALESCE(instansi,'') FROM admin_users WHERE instansi=$1 AND status='active' AND role ILIKE '%\"pengawas\"%' ORDER BY username",
          {inst});
        if(pw && PQresultStatus(pw.get())==PGRES_TUPLES_OK){
          std::string arr="[";
          for(int i=0;i<PQntuples(pw.get());i++){
            if(i>0) arr+=",";
            arr+="{\"id\":"+std::string(PQgetvalue(pw.get(),i,0))+",\"username\":\""+json_escape(PQgetvalue(pw.get(),i,1))+"\",\"instansi\":\""+json_escape(PQgetvalue(pw.get(),i,2))+"\"}";
          }
          arr+="]"; pengawas=arr;
        }
        // assigned pengawas ids
        auto ap=real.exec_params(c.get(),"SELECT user_id FROM exam_pengawas WHERE exam_id=$1",{std::to_string(id)});
        if(ap && PQresultStatus(ap.get())==PGRES_TUPLES_OK){
          std::string arr="[";
          for(int i=0;i<PQntuples(ap.get());i++){
            if(i>0) arr+=",";
            arr+=PQgetvalue(ap.get(),i,0);
          }
          arr+="]"; assigned=arr;
        }
      }
      real.release(c.release());
    }
  }catch(...){ /* best-effort */ }
#endif
  Response r; r.status=200; r.json(200,"{\"success\":true,\"data\":{"
    "\"current_owner\":"+current_owner
    +",\"delegated_to\":"+delegated_to
    +",\"available_gurus\":"+gurus
    +",\"available_pengawas\":"+pengawas
    +",\"assigned_pengawas_ids\":"+assigned+"}}"); return r;
}

Response delegate_exam(const Request& req){
  auto id_str=get_exam_id(req);
  if(id_str.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"exam id required\"}"); return r; }
  int id=0;
  try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"error\":\"invalid exam id\"}"); return r; }
  if(!exams().get_by_id(id)){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"exam not found\"}"); return r; }
  std::string new_owner_raw=json_raw_value(req.body,"new_owner_id");
  std::string pengawas_raw=json_raw_value(req.body,"pengawas_ids");
  std::vector<int> pengawas_ids;
  if(!pengawas_raw.empty() && !parse_int_array(pengawas_raw,pengawas_ids)){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"pengawas_ids must be an array of numbers\"}"); return r;
  }
  std::optional<int> new_owner;
  if(!new_owner_raw.empty()){
    try{ int v=std::stoi(new_owner_raw); if(v>0) new_owner=v; }catch(...){}
  }
#ifdef HAS_LIBPQ
  int uid=session_admin_id_from(req);
  try{
    auto cfg_db=Config::load();
    examvan::DbPool pool(cfg_db.database_url, 10);
    examvan::db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url), 10);
    if(auto c=real.acquire()){
      // validasi target guru: instansi sama, active, role guru (paritas Go)
      if(new_owner.has_value()){
        auto t=real.exec_params(c.get(),"SELECT COALESCE(instansi,''),role,status FROM admin_users WHERE id=$1",{std::to_string(*new_owner)});
        bool ok=t && PQresultStatus(t.get())==PGRES_TUPLES_OK && PQntuples(t.get())>0;
        if(ok){
          std::string ti=PQgetvalue(t.get(),0,0), tr=PQgetvalue(t.get(),0,1), ts=PQgetvalue(t.get(),0,2);
          auto ui=real.exec_params(c.get(),"SELECT instansi FROM admin_users WHERE id=$1",{std::to_string(uid)});
          std::string opinst=ui&&PQresultStatus(ui.get())==PGRES_TUPLES_OK&&PQntuples(ui.get())>0?PQgetvalue(ui.get(),0,0):"";
          ok = (ti==opinst && ts=="active" && tr.find("guru")!=std::string::npos);
        }
        if(!ok){
          Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"User tujuan harus Guru aktif di instansi yang sama\"}"); return r;
        }
      }
      real.exec_params(c.get(),"BEGIN",{});
      if(new_owner.has_value())
        real.exec_params(c.get(),"UPDATE exams SET delegated_to=$1 WHERE id=$2",{std::to_string(*new_owner),std::to_string(id)});
      else
        real.exec_params(c.get(),"UPDATE exams SET delegated_to=NULL WHERE id=$1",{std::to_string(id)});
      if(!pengawas_raw.empty()){
        real.exec_params(c.get(),"DELETE FROM exam_pengawas WHERE exam_id=$1",{std::to_string(id)});
        for(int pid: pengawas_ids){
          real.exec_params(c.get(),"INSERT INTO exam_pengawas (exam_id,user_id) VALUES ($1,$2)",{std::to_string(id),std::to_string(pid)});
        }
      }
      real.exec_params(c.get(),"COMMIT",{});
      real.release(c.release());
    }
  }catch(...){ utils::log_error("exam_delegate_failed","id="+id_str); }
#endif
  // Tanpa PG: best-effort via store (delegated_to tersimpan in-memory).
  if(new_owner.has_value()){
    exams().update(id,[&](models::Exam& e){ e.delegated_to=new_owner; });
  }
  utils::log_info("exam_delegated","id="+id_str);
  Response r; r.status=200; r.json(200,"{\"success\":true,\"ok\":true,\"id\":"+id_str+",\"message\":\"Delegasi ujian berhasil disimpan\"}"); return r;
}
} // namespace examvan::handlers::admin
