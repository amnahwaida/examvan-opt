#include "handlers/api/webhook.hpp"
#include "helpers/utils.hpp"
#include "middleware/protobuf.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#ifdef HAS_LIBPQ
#include "db/pool_real.hpp"
#include "db/pool.hpp"
#include "config/config.hpp"
#endif
#include <cctype>
#include <algorithm>

namespace examvan::handlers::api {

namespace {

// Ambil field string dari body JSON (mendukung string value saja).
std::string json_field_w(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t n=body.size();
  bool in_str=false; bool esc=false;
  for(size_t i=0;i<n;){
    if(!in_str && !esc && i+needle.size()<=n && body.compare(i,needle.size(),needle)==0){
      size_t c=i+needle.size();
      while(c<n && (body[c]==' '||body[c]=='\t'||body[c]=='\n'||body[c]=='\r')) c++;
      if(c<n && body[c]==':'){
        size_t v=c+1;
        while(v<n && (body[v]==' '||body[v]=='\t'||body[v]=='\n'||body[v]=='\r')) v++;
        if(v<n && body[v]=='"'){
          size_t q=v; size_t e=q+1;
          while(e<n){ if(body[e]=='\\'){e+=2;continue;} if(body[e]=='"') break; e++; }
          if(e<n) return body.substr(q+1,e-q-1);
        }
      }
    }
    char ch=body[i];
    if(esc) esc=false;
    else if(ch=='\\' && in_str) esc=true;
    else if(ch=='"') in_str=!in_str;
    i++;
  }
  return "";
}

bool contains_ci(const std::string& hay, const std::string& needle){
  if(needle.size()>hay.size()) return false;
  for(size_t i=0;i+needle.size()<=hay.size();++i){
    bool eq=true;
    for(size_t j=0;j<needle.size() && eq;++j)
      if(std::tolower((unsigned char)hay[i+j])!=std::tolower((unsigned char)needle[j])) eq=false;
    if(eq) return true;
  }
  return false;
}

// Ambil username & kode dari pesan, paritas Go regex:
//   (?i)username:\s*([a-zA-Z0-9_.-]+)  dan  (?i)kode:\s*([a-zA-Z0-9]+)
std::string capture_after_ci(const std::string& msg, const std::string& label){
  std::string lower=msg;
  for(char& c: lower) c=(char)std::tolower((unsigned char)c);
  std::string needle=label+":";
  std::string lneedle=needle;
  for(char& c: lneedle) c=(char)std::tolower((unsigned char)c);
  size_t p=lower.find(lneedle);
  if(p==std::string::npos) return "";
  size_t i=p+needle.size();
  while(i<msg.size() && (msg[i]==' '||msg[i]=='\t')) i++;
  std::string out;
  for(; i<msg.size(); ++i){
    char c=msg[i];
    bool ok=std::isalnum((unsigned char)c) || c=='_' || c=='.' || c=='-';
    if(!ok) break;
    out+=c;
  }
  return out;
}

#ifdef HAS_LIBPQ
// Normalisasi nomor HP (paritas Go normalizePhoneNumber): ambil digit saja,
// awalan "0" diganti "62".
std::string normalize_phone(const std::string& num){
  std::string d;
  for(char c: num) if(std::isdigit((unsigned char)c)) d+=c;
  if(!d.empty() && d[0]=='0') d="62"+d.substr(1);
  return d;
}
#endif // HAS_LIBPQ

Response respond(bool ok, const std::string& message){
  Response r; r.status=200;
  r.json(200,"{\"status\":"+std::string(ok?"true":"false")+",\"message\":\""+message+"\"}");
  return r;
}

} // namespace

Response webhook(const Request& req){
  if(req.body.empty()){
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::WebhookResponse pb; pb.set_success(false); pb.set_status("empty");
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=400; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    Response r; r.status=400; r.json(400,"{\"status\":false,\"message\":\"Payload tidak lengkap\"}"); return r;
  }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    // Saluran protobuf C++-native: ack sederhana (kontrak WebhookResponse).
    examvan::v1::WebhookResponse pb; pb.set_success(true); pb.set_status("ok");
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif

  // Parse sender/message: JSON dulu, lalu form-urlencoded.
  std::string sender=json_field_w(req.body,"sender");
  std::string message=json_field_w(req.body,"message");
  if(sender.empty() || message.empty()){
    auto form=helpers::parse_form(req.body);
    auto fs=form.find("sender"); if(fs!=form.end() && sender.empty()) sender=fs->second;
    auto fm=form.find("message"); if(fm!=form.end() && message.empty()) message=fm->second;
  }
  if(sender.empty() || message.empty()) return respond(false,"Payload tidak lengkap");

  if(!contains_ci(message,"verifikasi pendaftaran"))
    return respond(false,"Pesan bukan verifikasi pendaftaran");

  std::string username=capture_after_ci(message,"username");
  std::string code=capture_after_ci(message,"kode");
  if(username.empty() || code.empty())
    return respond(false,"Format pesan tidak sesuai template");

#ifdef HAS_LIBPQ
  try{
    auto cfg=Config::load();
    std::string db=cfg.database_url;
    if(db.empty()) if(auto* e=getenv("DATABASE_URL")) db=e;
    if(!db.empty()){
      std::string ci=pg_conninfo_from_url(db);
      if(ci.empty()) ci=db;
      examvan::db::RealPool real(ci, 2);
      if(real.connect()){
        if(auto c=real.acquire()){
          auto res=real.exec_params(c.get(),
            "SELECT id, username, whatsapp_number, status, otp_code FROM admin_users"
            " WHERE LOWER(username)=LOWER($1) AND otp_code=$2 AND status='pending_otp'",
            {username, code});
          if(res && PQresultStatus(res.get())==PGRES_TUPLES_OK && PQntuples(res.get())>0){
            std::string reg_phone=PQgetvalue(res.get(),0,2);
            if(normalize_phone(sender)!=normalize_phone(reg_phone))
              return respond(false,"Nomor WhatsApp pengirim tidak cocok dengan yang didaftarkan");
            auto up=real.exec_params(c.get(),
              "UPDATE admin_users SET status='active', otp_code=NULL, otp_expiry=NULL WHERE id=$1",
              {PQgetvalue(res.get(),0,0)});
            if(up && (PQresultStatus(up.get())==PGRES_COMMAND_OK||PQresultStatus(up.get())==PGRES_TUPLES_OK))
              return respond(true,"Verifikasi sukses! Akun Anda telah aktif.");
            Response r; r.status=500;
            r.json(500,"{\"status\":false,\"message\":\"Gagal mengaktifkan akun\"}"); return r;
          }
          return respond(false,"Akun tidak ditemukan atau kode kedaluwarsa");
        }
      }
    }
  }catch(...){}
#endif
  // Tanpa PG → paritas Go (pool==nil): 500 Database error.
  Response r; r.status=500;
  r.json(500,"{\"status\":false,\"message\":\"Database error\"}"); return r;
}

} // namespace examvan::handlers::api