#include "handlers/api/webhook.hpp"
#include "handlers/auth/auth_store.hpp"
#include "helpers/utils.hpp"
#include "middleware/protobuf.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#ifdef HAS_LIBPQ
#include "db/pool_real.hpp"
#include "db/pool.hpp"
#include "db/pool_global.hpp"
#include "config/config.hpp"
#endif
#include <cctype>
#include <algorithm>
#include <ctime>

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

  /* P33-F1: ack (success=true) HANYA setelah validasi sukses — saluran
   * protobuf tidak lagi meng-ack buta sebelum parse/DB. Semua jalur keluar
   * sadar-kanal: protobuf → WebhookResponse, JSON → respond(). */
  [[maybe_unused]] auto respond_channel=[&](bool ok,const std::string& msg)->Response{
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::WebhookResponse pb; pb.set_success(ok); pb.set_status(msg);
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    return respond(ok,msg);
  };
  /* P33-F1: PG dikonfigurasi tapi tidak terjangkau → fail-closed
   * (protobuf: success=false "Database tidak tersedia"; JSON: 503). */
  [[maybe_unused]] auto fail_closed_db=[&]()->Response{
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::WebhookResponse pb; pb.set_success(false); pb.set_status("Database tidak tersedia");
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    Response r; r.status=503; r.json(503,"{\"status\":false,\"message\":\"Database tidak tersedia\"}"); return r;
  };

  // Parse sender/message: JSON dulu, lalu form-urlencoded.
  std::string sender=json_field_w(req.body,"sender");
  std::string message=json_field_w(req.body,"message");
  if(sender.empty() || message.empty()){
    auto form=helpers::parse_form(req.body);
    auto fs=form.find("sender"); if(fs!=form.end() && sender.empty()) sender=fs->second;
    auto fm=form.find("message"); if(fm!=form.end() && message.empty()) message=fm->second;
  }
  if(sender.empty() || message.empty()) return respond_channel(false,"Payload tidak lengkap");

  if(!contains_ci(message,"verifikasi pendaftaran"))
    return respond_channel(false,"Pesan bukan verifikasi pendaftaran");

  std::string username=capture_after_ci(message,"username");
  std::string code=capture_after_ci(message,"kode");
  if(username.empty() || code.empty())
    return respond_channel(false,"Format pesan tidak sesuai template");

#ifdef HAS_LIBPQ
  try{
    auto cfg=Config::load();
    std::string db=cfg.database_url;
    if(db.empty()) if(auto* e=getenv("DATABASE_URL")) db=e;
    if(!db.empty()){
      /* P33-F1: pool proses-wide (bukan RealPool stack-lokal per request).
       * Lookup PER-USER (bukan scan seluruh admin_users), dengan cek expiry
       * + attempts OTP (paritas recovery.cpp) sebelum aktivasi. */
      bool pg_ok=false;
      Response out{};               // P33-F1: respons dibangun di lambda lalu dikembalikan
      examvan::db::with_global_pg([&](examvan::db::RealPool& real){
        auto c=real.acquire();
        if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
        pg_ok=true;
        auto finish=[&](Response rr){ out=std::move(rr); };
        auto res=real.exec_params(c.get(),
          "SELECT id, whatsapp_number, status, COALESCE(otp_code,''), COALESCE(otp_attempts,0), COALESCE(EXTRACT(EPOCH FROM otp_expiry)::bigint,0)"
          " FROM admin_users WHERE LOWER(username)=LOWER($1) LIMIT 1",
          {username});
        if(!res || PQresultStatus(res.get())!=PGRES_TUPLES_OK || PQntuples(res.get())==0){
          real.release(c.release());
          finish(respond_channel(false,"Akun tidak ditemukan atau kode kedaluwarsa"));
          return;
        }
        std::string reg_phone=PQgetvalue(res.get(),0,1);
        std::string status=PQgetvalue(res.get(),0,2);
        std::string otp_code=PQgetvalue(res.get(),0,3);
        long expiry_epoch=std::atol(PQgetvalue(res.get(),0,5));
        /* P37-F12: JANGAN release di sini — koneksi masih dipakai UPDATE
         * aktivasi di bawah (P33-F1 release prematur → exec di koneksi null;
         * PQexecParams(nullptr) silent-fail → setiap OTP valid berakhir
         * "Gagal mengaktifkan akun"). Satu release tunggal setelah UPDATE. */
        if(status!="pending_otp"){
          finish(respond_channel(false,"Akun tidak ditemukan atau kode kedaluwarsa"));
          return;
        }
        // P33-F1: OTP kedaluwarsa wajib ditolak (sebelumnya tanpa cek usia).
        long now=(long)std::time(nullptr);
        if(expiry_epoch>0 && now>expiry_epoch){
          auth::update_user_otp(username,"",0); // nonaktifkan OTP kedaluwarsa
          finish(respond_channel(false,"Kode OTP sudah kedaluwarsa."));
          return;
        }
        // P33-F1: kode salah → bump attempts (atomik via auth_store, M6);
        // lockout setelah 5x salah (anti brute-force sustained, paritas M6).
        if(code!=otp_code){
          int n=auth::bump_otp_attempts(username);
          if(n>=5){
            auth::update_user_otp(username,"",0); // nonaktifkan OTP setelah 5x salah
            finish(respond_channel(false,"Terlalu banyak percobaan. Minta kode baru."));
            return;
          }
          finish(respond_channel(false,"Akun tidak ditemukan atau kode kedaluwarsa"));
          return;
        }
        if(normalize_phone(sender)!=normalize_phone(reg_phone)){
          finish(respond_channel(false,"Nomor WhatsApp pengirim tidak cocok dengan yang didaftarkan"));
          return;
        }
        auto up=real.exec_params(c.get(),
          "UPDATE admin_users SET status='active', otp_code=NULL, otp_expiry=NULL WHERE LOWER(username)=LOWER($1)",
          {username});
        bool ok=up && (PQresultStatus(up.get())==PGRES_COMMAND_OK||PQresultStatus(up.get())==PGRES_TUPLES_OK);
        real.release(c.release()); // P37-F12: satu release tunggal setelah UPDATE
        if(ok){
          finish(respond_channel(true,"Verifikasi sukses! Akun Anda telah aktif."));
          return;
        }
        finish(respond_channel(false,"Gagal mengaktifkan akun"));
      });
      if(!pg_ok){ return fail_closed_db(); }
      if(out.status!=0) return out;   // jalur selesai (sukses/gagal validasi)
      return fail_closed_db();        // defensif: lambda keluar tanpa respons
    }
  }catch(...){}
  /* P33-F1: PG dikonfigurasi tapi exception/down → fail-closed. */
  {
    auto cfg=Config::load();
    std::string db=cfg.database_url;
    if(db.empty()) if(auto* e=getenv("DATABASE_URL")) db=e;
    if(!db.empty()) return fail_closed_db();
  }
#endif
  // Tanpa PG → paritas Go (pool==nil): 500 Database error (JSON).
  // P33-F1: saluran protobuf fail-closed — bukan ack buta success=true.
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::WebhookResponse pb; pb.set_success(false); pb.set_status("Database tidak tersedia");
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.status=500;
  r.json(500,"{\"status\":false,\"message\":\"Database error\"}"); return r;
}

} // namespace examvan::handlers::api