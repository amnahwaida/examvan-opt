#include "handlers/admin/settings.hpp"
#include "handlers/admin/template_helper.hpp"
#include "helpers/utils.hpp"
#include "config/config.hpp"
#include "middleware/protobuf.hpp"
#include "utils/log.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#ifdef HAS_LIBPQ
#include "db/pool.hpp"
#include "db/pool_real.hpp"
#include <libpq-fe.h>
#include <functional>
#endif
#include <map>
#include <string>

namespace examvan::handlers::admin {

// ===== helper JSON ========================================================
// Cek apakah sebuah key HADIR di body JSON (nilai apa pun, termasuk null).
// Partial update: hanya key yang hadir yang ditulis — menyimpan satu seksi
// (mis. Turnstile) tidak boleh mereset seksi lain ke default (paritas Go
// pointer fields: only present keys are written).
static bool json_has_key(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t n=body.size(); bool in_str=false, esc=false;
  for(size_t i=0;i<n;){
    if(!in_str && !esc && i+needle.size()<=n && body.compare(i,needle.size(),needle)==0){
      size_t c=i+needle.size();
      while(c<n && (body[c]==' '||body[c]=='\t'||body[c]=='\n'||body[c]=='\r')) c++;
      if(c<n && body[c]==':') return true;
    }
    char ch=body[i];
    if(esc) esc=false;
    else if(ch=='\\' && in_str) esc=true;
    else if(ch=='"') in_str=!in_str;
    i++;
  }
  return false;
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

// Mask rahasia saat GET: tampilkan 4 karakter terakhir (paritas Go maskTokenSetting).
static std::string mask_token(const std::string& t){
  if(t.size()<=8) return t;
  return std::string(t.size()-4,'*')+t.substr(t.size()-4);
}

#ifdef HAS_LIBPQ
static void with_pg(const std::function<void(examvan::db::RealPool&)>& fn){
  auto cfg=Config::load();
  std::string db_url=cfg.database_url;
  if(db_url.empty()) if(auto* e=getenv("DATABASE_URL")) db_url=e;
  if(db_url.empty()) return;
  examvan::DbPool pool(db_url, 10);
  examvan::db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url), 10);
  auto c=real.acquire();
  if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
  fn(real);
  real.release(c.release());
}

// Muat seluruh setting dari saas_settings ke map (key → value).
static std::map<std::string,std::string> load_all_settings(){
  std::map<std::string,std::string> m;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto r=real.exec_params(c.get(),"SELECT key,value FROM saas_settings",{});
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      for(int i=0;i<PQntuples(r.get());i++){
        m[PQgetvalue(r.get(),i,0)]=PQgetvalue(r.get(),i,1);
      }
    }
    real.release(c.release());
  });
  return m;
}

static void upsert_setting(const std::string& key, const std::string& value){
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto r=real.exec_params(c.get(),
      "INSERT INTO saas_settings (key,value) VALUES ($1,$2) ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value",
      {key,value});
    if(!r || (PQresultStatus(r.get())!=PGRES_COMMAND_OK && PQresultStatus(r.get())!=PGRES_TUPLES_OK)){
      utils::log_error("saas_settings_upsert_failed",PQresultErrorMessage(r.get()));
    }
    real.release(c.release());
  });
}
#endif

Response settings_page(const Request& req){
  if(req.path.find("/api/")!=std::string::npos){
    if(req.path.find("system-apps")!=std::string::npos){
#ifdef HAS_PROTOBUF
      if(middleware::is_protobuf_accept(req)){
        examvan::v1::VoucherList pb; pb.set_success(true);
        std::string out; pb.SerializeToString(&out);
        Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
      }
#endif
      Response r; r.json(200,"{\"success\":true,\"apps\":[],\"system_apps\":[]}"); return r;
    }
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::Settings pb; pb.set_success(true);
      pb.set_smtp_host("smtp.gmail.com"); pb.set_smtp_port(587);
      pb.set_default_max_exams(3); pb.set_default_max_pdf_size_mb(1);
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    // Baca dari PG (kalau tersedia), fallback default — nilai dari DB menang.
    std::map<std::string,std::string> s;
#ifdef HAS_LIBPQ
    s=load_all_settings();
#endif
    auto get=[&](const std::string& k, const std::string& d)->std::string{
      auto it=s.find(k); return it==s.end()? d : it->second;
    };
    auto mask=[&](const std::string& k)->std::string{
      auto it=s.find(k); return it==s.end()? "" : mask_token(it->second);
    };
    std::string json=std::string("{\"success\":true,\"settings\":{")
      +"\"email_verification_enabled\":"+(get("email_verification_enabled","0")=="1"?"true":"false")
      +",\"email_domain_whitelist\":\""+json_escape(get("email_domain_whitelist",""))+"\""
      +",\"smtp_host\":\""+json_escape(get("smtp_host","smtp.gmail.com"))+"\""
      +",\"smtp_port\":"+get("smtp_port","587")
      +",\"smtp_user\":\""+json_escape(get("smtp_user",""))+"\""
      +",\"smtp_password\":\""+json_escape(mask("smtp_password"))+"\""
      +",\"smtp_sender_name\":\""+json_escape(get("smtp_sender_name","EXAMVAN"))+"\""
      +",\"default_max_exams\":"+get("default_max_exams","3")
      +",\"default_max_concurrent_exams\":"+get("default_max_concurrent_exams","2")
      +",\"default_max_pdf_size_mb\":"+get("default_max_pdf_size_mb","1")
      +",\"default_max_storage_size_mb\":"+get("default_max_storage_size_mb","50")
      +",\"storage_free_mb\":10240"
      +",\"default_active_days\":"+get("default_active_days","14")
      +",\"android_version\":\""+json_escape(get("android_version","2.1.9"))+"\""
      +",\"webapp_version\":\""+json_escape(get("webapp_version","2.1.9"))+"\""
      +",\"certificate_fingerprint\":\""+json_escape(get("certificate_fingerprint",""))+"\""
      +",\"seo_title\":\""+json_escape(get("seo_title",""))+"\""
      +",\"seo_description\":\""+json_escape(get("seo_description",""))+"\""
      +",\"seo_keywords\":\""+json_escape(get("seo_keywords",""))+"\""
      +",\"seo_index\":"+(get("seo_index","0")=="1"?"true":"false")
      +",\"footer_text\":\""+json_escape(get("footer_text","\u00a9 2026 EXAMVAN Team. All rights reserved."))+"\""
      +",\"footer_tagline\":\""+json_escape(get("footer_tagline",""))+"\""
      +",\"voucher_redeem_enabled\":"+(get("voucher_redeem_enabled","1")!="0"?"true":"false")
      +",\"turnstile_enabled\":"+(get("turnstile_enabled","0")=="1"?"true":"false")
      +",\"turnstile_site_key\":\""+json_escape(get("turnstile_site_key",""))+"\""
      +",\"turnstile_secret_key\":\""+json_escape(mask("turnstile_secret_key"))+"\""
      +",\"max_accounts_per_ip\":"+get("max_accounts_per_ip","3")
      +",\"max_approvals_per_exam\":"+get("max_approvals_per_exam","500")
      +",\"approval_cleanup_interval_minutes\":"+get("approval_cleanup_interval_minutes","15")
      +",\"approval_cleanup_ended_grace_hours\":"+get("approval_cleanup_ended_grace_hours","1")
      +",\"approval_cleanup_inactive_ttl_hours\":"+get("approval_cleanup_inactive_ttl_hours","24")
      +"}}";
    Response r; r.json(200,json); return r;
  }
  RenderedAdminPage rp=render_admin_page("settings","2.7.2");
  if(!rp.html.empty()){
    // C5: set cookie CSRF agar token meta cocok dengan cookie yang diverifikasi.
    Response r; r.status=200; r.headers["Content-Type"]="text/html";
    if(!rp.csrf_cookie.empty()) r.headers["Set-Cookie"]=rp.csrf_cookie;
    r.body=rp.html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Settings</h1></body></html>"; return r;
}

// POST /admin/api/saas-settings — partial update: hanya key yang HADIR di
// body yang di-UPSERT (paritas Go pointer fields). Key yang absen tidak
// disentuh, sehingga menyimpan seksi SMTP tidak mereset Turnstile/SEO.
static const char* kSettingKeys[]={
  "email_verification_enabled","email_domain_whitelist","smtp_host","smtp_port",
  "smtp_user","smtp_password","smtp_sender_name","default_max_exams",
  "default_max_pdf_size_mb","default_max_storage_size_mb","default_max_concurrent_exams",
  "default_active_days","android_version","webapp_version","certificate_fingerprint",
  "seo_title","seo_description","seo_keywords","seo_index","footer_text",
  "footer_tagline","voucher_redeem_enabled","turnstile_enabled","turnstile_site_key",
  "turnstile_secret_key","max_accounts_per_ip","max_approvals_per_exam",
  "approval_cleanup_interval_minutes","approval_cleanup_ended_grace_hours",
  "approval_cleanup_inactive_ttl_hours","access_log_retention_days",
  "access_log_retention_interval_minutes"
};
static const int kSettingKeysCount = sizeof(kSettingKeys)/sizeof(kSettingKeys[0]);

Response update_settings(const Request& req){
  // Parsing nilai per key: string_field menangani angka tanpa quote? Tidak —
  // ambil nilai mentah lalu normalisasi (boleh ber-quote atau tidak).
  auto raw_val=[&](const std::string& key)->std::string{
    std::string needle="\""+key+"\"";
    size_t p=req.body.find(needle);
    if(p==std::string::npos) return "";
    size_t colon=req.body.find(':',p+needle.size());
    if(colon==std::string::npos) return "";
    size_t s=req.body.find_first_not_of(" \t\r\n",colon+1);
    if(s==std::string::npos) return "";
    if(req.body[s]=='"') return json_string_field(req.body,key);
    size_t e=s;
    while(e<req.body.size() && req.body[e]!=',' && req.body[e]!='}' && req.body[e]!='\n' && req.body[e]!='\r') e++;
    std::string v=req.body.substr(s,e-s);
    size_t a=v.find_first_not_of(" \t\r\n"); size_t b=v.find_last_not_of(" \t\r\n");
    if(a==std::string::npos) return "";
    return v.substr(a,b-a+1);
  };
  int written=0;
  for(int i=0;i<kSettingKeysCount;i++){
    std::string key=kSettingKeys[i];
    if(!json_has_key(req.body,key)) continue; // only present keys are written
    std::string value=raw_val(key);
    // GET masks secrets as ***...last4; accepting that mask would destroy
    // the stored secret when the frontend saves unrelated settings.
    if((key=="smtp_password" || key=="turnstile_secret_key") && value.find('*')!=std::string::npos){
      continue;
    }
#ifdef HAS_LIBPQ
    upsert_setting(key,value);
    written++;
#else
    (void)value; written++;
#endif
  }
  if(written==0){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"tidak ada setting yang valid\"}"); return r;
  }
  utils::log_info("saas_settings_updated","keys="+std::to_string(written));
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Pengaturan berhasil disimpan\"}"); return r;
}

Response system_apps_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>System Apps</h1></body></html>"; return r;
}
} // namespace examvan::handlers::admin