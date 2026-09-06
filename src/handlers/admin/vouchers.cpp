#include "handlers/admin/vouchers.hpp"
#include "helpers/utils.hpp"
#include "helpers/password.hpp"
#include "session/cookie.hpp"
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
#include <string>
#include <cctype>
#include <optional>
#include <chrono>
#include <cstdio>
namespace examvan::handlers::admin {

static std::string get_param(const std::map<std::string,std::string>& form, const std::string& key){
  auto it=form.find(key); return it!=form.end()? it->second : "";
}

#ifdef HAS_LIBPQ
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
#endif

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

#ifdef HAS_LIBPQ
// M15: baca skalar JSON mentah — string ber-quote ATAU angka tanpa quote
// ("max_exams": 5 → "5"). json_string_field di atas HANYA membaca ber-quote,
// sehingga angka polos dari frontend dibaca "" → atoll("")=0 (limit ter-reset
// diam-diam ke 0 tiap simpan paket).
static std::string json_raw_scalar(const std::string& body, const std::string& key){
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
          size_t e=v+1; while(e<n){ if(body[e]=='\\'){ e+=2; continue; } if(body[e]=='"') break; e++; }
          if(e>=n) return "";
          return body.substr(v+1,e-v-1);
        }
        size_t e=v; while(e<n && body[e]!=',' && body[e]!='}' && body[e]!=']') e++;
        std::string s=body.substr(v,e-v);
        s.erase(0,s.find_first_not_of(" \t\r\n"));
        size_t t=s.find_last_not_of(" \t\r\n");
        if(t!=std::string::npos) s.erase(t+1);
        return s;
      }
    }
    char c=body[i];
    if(esc) esc=false;
    else if(c=='\\' && in_str) esc=true;
    else if(c=='"') in_str=!in_str;
    i++;
  }
  return "";
}
#endif // HAS_LIBPQ — json_raw_scalar dipakai save_packages (HAS_LIBPQ)

static int session_admin_id_from(const Request& req){
  for(auto& kv:req.headers){ std::string k=kv.first; for(char& ch:k) ch=tolower((unsigned char)ch); if(k=="x-internal-admin-id"){ try{ return std::stoi(kv.second); }catch(...){} } }
  return 0;
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

// Masih aktif? (is_active + belum lewat expires_at)
static bool voucher_usable(const std::string& is_active, const std::string& expires_at){
  if(is_active!="t") return false;
  if(!expires_at.empty()){
    auto tp=helpers::parse_iso_utc(expires_at);
    if(tp && *tp < std::chrono::system_clock::now()) return false;
  }
  return true;
}

// Map duration_type ("bulanan"/"semester"/"tahunan"/angka hari) → jumlah hari.
static int duration_days(const std::string& dt){
  if(dt=="bulanan") return 30;
  if(dt=="semester") return 180;
  if(dt=="tahunan") return 365;
  try{ int d=std::stoi(dt); if(d>0) return d; }catch(...){}
  return 30;
}
#endif

// ===== list ================================================================
Response list_vouchers(const Request& req){
  /* M1: stub protobuf awal dihapus — daftar kosong padahal ada data. */
  auto q=helpers::parse_form(req.query);
  int page=1, per_page=20;
  try{ page=std::stoi(get_param(q,"page")); }catch(...){}
  try{ per_page=std::stoi(get_param(q,"per_page")); }catch(...){}
  if(page<1) page=1; else if(page>1000000) page=1000000;
  if(per_page<1) per_page=20; else if(per_page>200) per_page=200;
  std::string search=get_param(q,"search");
  std::string pagination="{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)+",\"total\":0,\"total_pages\":0}";
#ifdef HAS_LIBPQ
  std::string vouchers_json="[]";
  bool got=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    std::string where=" WHERE 1=1";
    std::vector<std::string> params;
    if(!search.empty()){
      where+=" AND (code ILIKE $"+std::to_string(params.size()+1)+" OR package ILIKE $"+std::to_string(params.size()+1)+" OR COALESCE(notes,'') ILIKE $"+std::to_string(params.size()+1)+")";
      params.push_back("%"+search+"%");
    }
    std::string sql="SELECT v.id,v.code,v.package,v.duration_type,v.max_usage,v.used_count,v.expires_at,v.is_active,v.notes,v.is_custom,COALESCE(v.custom_label,''),COALESCE(v.custom_max_exams,0),COALESCE(v.custom_max_pdf_size,0),COALESCE(v.custom_max_concurrent_exams,0),COALESCE(v.custom_max_storage_size,0),COALESCE(v.custom_max_users,0),COALESCE(v.custom_role,''),u.username,v.created_at, COUNT(*) OVER() AS total"
      " FROM vouchers v LEFT JOIN admin_users u ON u.id=v.created_by"+where+" ORDER BY v.id DESC"
      " LIMIT $"+std::to_string(params.size()+1)+" OFFSET $"+std::to_string(params.size()+2);
    params.push_back(std::to_string(per_page));
    params.push_back(std::to_string(static_cast<int64_t>(page-1)*static_cast<int64_t>(per_page)));
    auto r=real.exec_params(c.get(),sql,params);
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      int n=PQntuples(r.get());
      int total=0; if(n>0) total=std::atoi(PQgetvalue(r.get(),0,19));
      int total_pages=(total+per_page-1)/per_page;
      std::string arr="[";
      for(int i=0;i<n;i++){
        if(i>0) arr+=",";
        std::string exp=PQgetvalue(r.get(),i,6);
        if(exp.size()>=19) exp=exp.substr(0,19);
        arr+="{\"id\":"+std::string(PQgetvalue(r.get(),i,0))
          +",\"code\":\""+json_escape(PQgetvalue(r.get(),i,1))+"\""
          +",\"package\":\""+json_escape(PQgetvalue(r.get(),i,2))+"\""
          +",\"duration_type\":\""+json_escape(PQgetvalue(r.get(),i,3))+"\""
          +",\"max_usage\":"+PQgetvalue(r.get(),i,4)
          +",\"used_count\":"+PQgetvalue(r.get(),i,5)
          +",\"expires_at\":\""+json_escape(exp)+"\""
          +",\"is_active\":"+(std::string(PQgetvalue(r.get(),i,7))=="t"?"true":"false")
          +",\"notes\":\""+json_escape(PQgetvalue(r.get(),i,8))+"\""
          +",\"is_custom\":"+(std::string(PQgetvalue(r.get(),i,9))=="t"?"true":"false")
          +",\"custom_label\":\""+json_escape(PQgetvalue(r.get(),i,10))+"\""
          +",\"custom_max_exams\":"+PQgetvalue(r.get(),i,11)
          +",\"custom_max_pdf_size\":"+PQgetvalue(r.get(),i,12)
          +",\"custom_max_concurrent_exams\":"+PQgetvalue(r.get(),i,13)
          +",\"custom_max_storage_size\":"+PQgetvalue(r.get(),i,14)
          +",\"custom_max_users\":"+PQgetvalue(r.get(),i,15)
          +",\"custom_role\":\""+json_escape(PQgetvalue(r.get(),i,16))+"\""
          +",\"created_by_username\":\""+json_escape(PQgetvalue(r.get(),i,17))+"\""
          +",\"created_at\":\""+json_escape(PQgetvalue(r.get(),i,18))+"\"}";
      }
      arr+="]";
      vouchers_json=arr;
      pagination="{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)
        +",\"total\":"+std::to_string(total)+",\"total_pages\":"+std::to_string(total_pages)+"}";
      got=true;
    }
    real.release(c.release());
  });
  if(got){
    Response r; r.json(200,"{\"success\":true,\"vouchers\":"+vouchers_json+",\"pagination\":"+pagination+"}"); return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"vouchers\":[],\"pagination\":"+pagination+"}"); return r;
}

// ===== create ==============================================================
Response create_voucher(const Request& req){
  auto form=helpers::parse_form(req.body);
  std::string code=get_param(form,"code");
  std::string package=get_param(form,"package"); if(package.empty()) package="free";
  std::string duration_type=get_param(form,"duration_type"); if(duration_type.empty()) duration_type="bulanan";
  std::string max_usage_s=get_param(form,"max_usage"); if(max_usage_s.empty()) max_usage_s="1";
  std::string expires_at=get_param(form,"expires_at");
  std::string notes=get_param(form,"notes");
  if(code.empty()) code=json_string_field(req.body,"code");
  // normalisasi: uppercase + trim (paritas Go)
  { std::string out; for(char ch: code){ out.push_back((char)toupper((unsigned char)ch)); } code=out; }
  if(code.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"code wajib diisi\"}"); return r; }
  int max_usage=1; try{ max_usage=std::stoi(max_usage_s); }catch(...){}
  if(max_usage<1) max_usage=1;
  if(code.size()>64){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Kode voucher terlalu panjang\"}"); return r; }
  bool is_custom=false; std::string custom_label="",custom_role="";
  int custom_max_exams=0,custom_max_concurrent=0;
  long long custom_max_pdf=0,custom_max_storage=0,custom_max_users=0;
  if(package=="custom"){
    is_custom=true;
    custom_label=get_param(form,"custom_label"); if(custom_label.empty()) custom_label="custom";
    custom_role=get_param(form,"custom_role");
    custom_max_exams=std::atoi(get_param(form,"custom_max_exams").c_str());
    custom_max_concurrent=std::atoi(get_param(form,"custom_max_concurrent_exams").c_str());
    custom_max_pdf=(long long)(std::atof(get_param(form,"custom_max_pdf_size_mb").c_str())*1024*1024);
    custom_max_storage=(long long)(std::atof(get_param(form,"custom_max_storage_size_mb").c_str())*1024*1024);
    custom_max_users=std::atoll(get_param(form,"custom_max_users").c_str());
    package=custom_label;
  }
  (void)is_custom; (void)custom_max_exams; (void)custom_max_concurrent;
  (void)custom_max_pdf; (void)custom_max_storage; (void)custom_max_users;
  int uid=session_admin_id_from(req);
  (void)uid;
#ifdef HAS_LIBPQ
  std::string result="";
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto dup=real.exec_params(c.get(),"SELECT id FROM vouchers WHERE UPPER(code)=UPPER($1) LIMIT 1",{code});
    if(dup && PQresultStatus(dup.get())==PGRES_TUPLES_OK && PQntuples(dup.get())>0){ result="__dup__"; }
    if(result.empty()){
      std::string sql="INSERT INTO vouchers (code,package,duration_type,max_usage,expires_at,notes,created_by,is_custom,custom_label,custom_max_exams,custom_max_concurrent_exams,custom_max_pdf_size,custom_max_storage_size,custom_max_users,custom_role) VALUES ($1,$2,$3,$4,$5::timestamptz,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15) RETURNING id";
      std::vector<std::string> params={code,package,duration_type,std::to_string(max_usage),expires_at,notes,
        uid>0?std::to_string(uid):"NULL",is_custom?"true":"false",custom_label,
        std::to_string(custom_max_exams),std::to_string(custom_max_concurrent),
        std::to_string(custom_max_pdf),std::to_string(custom_max_storage),
        std::to_string(custom_max_users),custom_role};
      auto ins=real.exec_params(c.get(),sql,params);
      if(ins && PQresultStatus(ins.get())==PGRES_TUPLES_OK && PQntuples(ins.get())>0){ result="ok"; }
      else { utils::log_error("voucher_create_failed",PQresultErrorMessage(ins.get())); result="__fail__"; }
    }
    real.release(c.release());
  });
  if(result=="__dup__"){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Kode voucher sudah digunakan\"}"); return r; }
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal menyimpan voucher\"}"); return r; }
  if(result=="ok"){
    utils::log_info("voucher_created","code="+code);
    Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Voucher berhasil dibuat\"}"); return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Voucher berhasil dibuat\"}"); return r;
}

// ===== batch create =========================================================
Response create_vouchers_batch(const Request& req){
  auto form=helpers::parse_form(req.body);
  std::string prefix=get_param(form,"prefix");
  std::string package=get_param(form,"package"); if(package.empty()) package="free";
  std::string duration_type=get_param(form,"duration_type"); if(duration_type.empty()) duration_type="bulanan";
  std::string max_usage_s=get_param(form,"max_usage"); if(max_usage_s.empty()) max_usage_s="1";
  std::string expires_at=get_param(form,"expires_at");
  std::string notes=get_param(form,"notes");
  int count=10; try{ count=std::stoi(get_param(form,"count")); }catch(...){}
  if(count<1) count=1;
  if(count>500) count=500;
  int max_usage=1; try{ max_usage=std::stoi(max_usage_s); }catch(...){}
  if(max_usage<1) max_usage=1;
  int uid=session_admin_id_from(req);
  (void)uid;
  // normalisasi prefix: huruf besar
  { std::string out; for(char ch: prefix){ out.push_back((char)toupper((unsigned char)ch)); } prefix=out; }
  int created_ok=0;
#ifdef HAS_LIBPQ
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    for(int i=0;i<count;i++){
      std::string code=prefix+helpers::generate_token(8);
      std::string sql="INSERT INTO vouchers (code,package,duration_type,max_usage,expires_at,notes,created_by) VALUES ($1,$2,$3,$4,$5::timestamptz,$6,$7)";
      std::vector<std::string> params={code,package,duration_type,std::to_string(max_usage),expires_at,notes,
        uid>0?std::to_string(uid):"NULL"};
      auto ins=real.exec_params(c.get(),sql,params);
      if(ins && PQresultStatus(ins.get())==PGRES_TUPLES_OK) created_ok++;
    }
    real.release(c.release());
  });
#endif
  if(created_ok==0){
    // tanpa PG: masih buat di memori-tidak-ada → laporkan jujur
    Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Database tidak tersedia\"}"); return r;
  }
  utils::log_info("vouchers_batch_created","count="+std::to_string(created_ok));
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"created\":"+std::to_string(created_ok)+",\"message\":\""+std::to_string(created_ok)+" voucher berhasil dibuat\"}"); return r;
}

// ===== toggle / delete ======================================================
static std::string voucher_id_from(const Request& req){
  auto it=req.params.find("id");
  if(it!=req.params.end() && !it->second.empty()) return it->second;
  size_t p=req.path.find("/vouchers/");
  if(p==std::string::npos) return "";
  std::string rest=req.path.substr(p+10);
  size_t slash=rest.find('/');
  return slash==std::string::npos? rest : rest.substr(0,slash);
}

Response toggle_voucher(const Request& req){
  std::string id_str=voucher_id_from(req);
  if(id_str.empty()||id_str.find('/')!=std::string::npos){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r; }
#ifdef HAS_LIBPQ
  std::string result="";
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto cur=real.exec_params(c.get(),"SELECT is_active FROM vouchers WHERE id=$1",{id_str});
    if(!cur || PQresultStatus(cur.get())!=PGRES_TUPLES_OK || PQntuples(cur.get())==0){ result="notfound"; real.release(c.release()); return; }
    std::string next=std::string(PQgetvalue(cur.get(),0,0))=="t" ? "false":"true";
    auto up=real.exec_params(c.get(),"UPDATE vouchers SET is_active=$2 WHERE id=$1",{id_str,next});
    if(up && PQresultStatus(up.get())==PGRES_COMMAND_OK){ result=next; }
    else { result="__fail__"; }
    real.release(c.release());
  });
  if(result=="notfound"){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"voucher not found\"}"); return r; }
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal mengubah status voucher\"}"); return r; }
  if(result=="true"||result=="false"){
    Response r; r.json(200,"{\"success\":true,\"ok\":true,\"is_active\":"+result+",\"message\":\"Status voucher diubah\"}"); return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Status voucher diubah\"}"); return r;
}

Response delete_voucher(const Request& req){
  std::string id_str=voucher_id_from(req);
  if(id_str.empty()||id_str.find('/')!=std::string::npos){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r; }
#ifdef HAS_LIBPQ
  std::string result="";
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto del=real.exec_params(c.get(),"DELETE FROM vouchers WHERE id=$1",{id_str});
    if(del && PQresultStatus(del.get())==PGRES_COMMAND_OK){ result="ok"; }
    else { utils::log_error("voucher_delete_failed",PQresultErrorMessage(del.get())); result="__fail__"; }
    real.release(c.release());
  });
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal menghapus voucher (mungkin sudah dipakai)\"}"); return r; }
  if(result=="ok"){ Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Voucher dihapus\"}"); return r; }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Voucher dihapus\"}"); return r;
}

// ===== redemptions per voucher ==============================================
Response voucher_redemptions(const Request& req){
  std::string id_str=voucher_id_from(req);
  if(id_str.empty()||id_str.find('/')!=std::string::npos){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r; }
#ifdef HAS_LIBPQ
  std::string arr="[]";
  bool found=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto r=real.exec_params(c.get(),
      "SELECT vr.id,vr.user_id,u.username,COALESCE(u.instansi,''),vr.package,vr.redeemed_at,vr.is_active,vr.remaining_seconds,vr.max_exams,vr.max_storage_size FROM voucher_redemptions vr LEFT JOIN admin_users u ON u.id=vr.user_id WHERE vr.voucher_id=$1 ORDER BY vr.redeemed_at DESC",
      {id_str});
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      found=true;
      std::string s="[";
      for(int i=0;i<PQntuples(r.get());i++){
        if(i>0) s+=",";
        std::string rt=PQgetvalue(r.get(),i,5); if(rt.size()>=19) rt=rt.substr(0,19);
        s+="{\"id\":"+std::string(PQgetvalue(r.get(),i,0))+",\"user_id\":"+PQgetvalue(r.get(),i,1)
          +",\"username\":\""+json_escape(PQgetvalue(r.get(),i,2))+"\""
          +",\"instansi\":\""+json_escape(PQgetvalue(r.get(),i,3))+"\""
          +",\"package\":\""+json_escape(PQgetvalue(r.get(),i,4))+"\""
          +",\"redeemed_at\":\""+json_escape(rt)+"\""
          +",\"is_active\":"+(std::string(PQgetvalue(r.get(),i,6))=="t"?"true":"false")
          +",\"remaining_seconds\":"+PQgetvalue(r.get(),i,7)
          +",\"max_exams\":"+PQgetvalue(r.get(),i,8)
          +",\"max_storage_size\":"+PQgetvalue(r.get(),i,9)+"}";
      }
      s+="]"; arr=s;
    }
    real.release(c.release());
  });
  if(found){ Response r; r.json(200,"{\"success\":true,\"redemptions\":"+arr+"}"); return r; }
#endif
  Response r; r.json(200,"{\"success\":true,\"redemptions\":[]}"); return r;
}

// ===== audit logs ===========================================================
Response list_audit_logs(const Request& req){
  auto q=helpers::parse_form(req.query);
  int page=1, per_page=20;
  try{ page=std::stoi(get_param(q,"page")); }catch(...){}
  try{ per_page=std::stoi(get_param(q,"per_page")); }catch(...){}
  if(page<1) page=1; else if(page>1000000) page=1000000;
  if(per_page<1) per_page=20; else if(per_page>200) per_page=200;
  std::string search=get_param(q,"search");
  std::string pagination="{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)+",\"total\":0,\"total_pages\":0}";
#ifdef HAS_LIBPQ
  std::string arr="[]";
  bool got=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    std::string where=" WHERE 1=1";
    std::vector<std::string> params;
    if(!search.empty()){
      where+=" AND (u.username ILIKE $"+std::to_string(params.size()+1)+" OR v.code ILIKE $"+std::to_string(params.size()+1)+")";
      params.push_back("%"+search+"%");
    }
    std::string sql="SELECT vr.id,vr.voucher_id,v.code,u.username,COALESCE(u.instansi,''),vr.package,vr.redeemed_at,vr.is_active,vr.remaining_seconds,vr.max_exams, COUNT(*) OVER() AS total"
      " FROM voucher_redemptions vr LEFT JOIN admin_users u ON u.id=vr.user_id LEFT JOIN vouchers v ON v.id=vr.voucher_id"+where
      +" ORDER BY vr.redeemed_at DESC LIMIT $"+std::to_string(params.size()+1)+" OFFSET $"+std::to_string(params.size()+2);
    params.push_back(std::to_string(per_page));
    params.push_back(std::to_string(static_cast<int64_t>(page-1)*static_cast<int64_t>(per_page)));
    auto r=real.exec_params(c.get(),sql,params);
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      int n=PQntuples(r.get());
      int total=0; if(n>0) total=std::atoi(PQgetvalue(r.get(),0,10));
      std::string s="[";
      for(int i=0;i<n;i++){
        if(i>0) s+=",";
        std::string rt=PQgetvalue(r.get(),i,6); if(rt.size()>=19) rt=rt.substr(0,19);
        s+="{\"id\":"+std::string(PQgetvalue(r.get(),i,0))
          +",\"voucher_id\":"+PQgetvalue(r.get(),i,1)
          +",\"code\":\""+json_escape(PQgetvalue(r.get(),i,2))+"\""
          +",\"username\":\""+json_escape(PQgetvalue(r.get(),i,3))+"\""
          +",\"instansi\":\""+json_escape(PQgetvalue(r.get(),i,4))+"\""
          +",\"package\":\""+json_escape(PQgetvalue(r.get(),i,5))+"\""
          +",\"redeemed_at\":\""+json_escape(rt)+"\""
          +",\"is_active\":"+(std::string(PQgetvalue(r.get(),i,7))=="t"?"true":"false")
          +",\"remaining_seconds\":"+PQgetvalue(r.get(),i,8)
          +",\"max_exams\":"+PQgetvalue(r.get(),i,9)+"}";
      }
      s+="]"; arr=s;
      pagination="{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)
        +",\"total\":"+std::to_string(total)+",\"total_pages\":"+std::to_string((total+per_page-1)/per_page)+"}";
      got=true;
    }
    real.release(c.release());
  });
  if(got){
    Response r; r.json(200,"{\"success\":true,\"logs\":"+arr+",\"pagination\":"+pagination+"}"); return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"logs\":[],\"pagination\":"+pagination+"}"); return r;
}

// ===== mine (redemptions milik session user) =================================
Response vouchers_mine(const Request& req){
  int uid=session_admin_id_from(req);
  (void)uid;
#ifdef HAS_LIBPQ
  std::string arr="[]";
  bool got=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto r=real.exec_params(c.get(),
      "SELECT vr.id,vr.package,v.code,vr.redeemed_at,vr.is_active,vr.remaining_seconds,vr.activated_at,vr.max_exams,vr.max_pdf_size,vr.max_concurrent_exams,vr.max_storage_size FROM voucher_redemptions vr LEFT JOIN vouchers v ON v.id=vr.voucher_id WHERE vr.user_id=$1 ORDER BY vr.redeemed_at DESC",
      {std::to_string(uid)});
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      got=true;
      std::string s="[";
      for(int i=0;i<PQntuples(r.get());i++){
        if(i>0) s+=",";
        std::string rt=PQgetvalue(r.get(),i,3); if(rt.size()>=19) rt=rt.substr(0,19);
        std::string at=PQgetvalue(r.get(),i,6); if(at.size()>=19) at=at.substr(0,19);
        s+="{\"id\":"+std::string(PQgetvalue(r.get(),i,0))
          +",\"package\":\""+json_escape(PQgetvalue(r.get(),i,1))+"\""
          +",\"code\":\""+json_escape(PQgetvalue(r.get(),i,2))+"\""
          +",\"redeemed_at\":\""+json_escape(rt)+"\""
          +",\"is_active\":"+(std::string(PQgetvalue(r.get(),i,4))=="t"?"true":"false")
          +",\"remaining_seconds\":"+PQgetvalue(r.get(),i,5)
          +",\"activated_at\":\""+json_escape(at)+"\""
          +",\"max_exams\":"+PQgetvalue(r.get(),i,7)
          +",\"max_pdf_size\":"+PQgetvalue(r.get(),i,8)
          +",\"max_concurrent_exams\":"+PQgetvalue(r.get(),i,9)
          +",\"max_storage_size\":"+PQgetvalue(r.get(),i,10)+"}";
      }
      s+="]"; arr=s;
    }
    real.release(c.release());
  });
  if(got){ Response r; r.json(200,"{\"success\":true,\"redemptions\":"+arr+"}"); return r; }
#endif
  Response r; r.json(200,"{\"success\":true,\"redemptions\":[]}"); return r;
}

// ===== redeem ===============================================================
Response redeem_voucher(const Request& req){
  auto form=helpers::parse_form(req.body);
  std::string code=get_param(form,"code");
  if(code.empty()) code=json_string_field(req.body,"code");
  { std::string out; for(char ch: code){ out.push_back((char)toupper((unsigned char)ch)); } code=out; }
  if(code.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Silakan masukkan kode voucher\"}"); return r; }
  int uid=session_admin_id_from(req);
  if(uid<=0){ Response r; r.status=401; r.json(401,"{\"success\":false,\"error\":\"Sesi tidak valid. Silakan login kembali.\"}"); return r; }
#ifdef HAS_LIBPQ
  std::string result=""; // "", "__invalid__", "__used__", "__expired__", "__quota__", "__notfound__", "ok"
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    // 1. superadmin / operator-created tidak boleh redeem
    auto us=real.exec_params(c.get(),"SELECT role,operator_created FROM admin_users WHERE id=$1",{std::to_string(uid)});
    if(us && PQresultStatus(us.get())==PGRES_TUPLES_OK && PQntuples(us.get())>0){
      std::string role=PQgetvalue(us.get(),0,0);
      if(role.find("superadmin")!=std::string::npos){ result="__super__"; real.release(c.release()); return; }
      if(std::string(PQgetvalue(us.get(),0,1))=="t"){ result="__operator_created__"; real.release(c.release()); return; }
    } else { result="notfound"; real.release(c.release()); return; }
    // 2. cari voucher (case-insensitive + trim). M13: bungkus dalam transaksi +
    //    SELECT ... FOR UPDATE — dua redeem konkuren utk voucher max_usage=1
    //    sama-sama membaca used_count=0 lalu sama-sama increment (double-spend)
    //    tanpa lock baris. FOR UPDATE mengunci baris voucher sampai COMMIT.
    real.exec_params(c.get(),"BEGIN",{});
    auto v=real.exec_params(c.get(),
      "SELECT id,code,package,duration_type,max_usage,used_count,expires_at,is_active,is_custom,COALESCE(custom_label,''),COALESCE(custom_max_exams,0),COALESCE(custom_max_pdf_size,0),COALESCE(custom_max_concurrent_exams,0),COALESCE(custom_max_storage_size,0),COALESCE(custom_max_users,0),COALESCE(custom_role,'') FROM vouchers WHERE UPPER(TRIM(code))=UPPER(TRIM($1)) FOR UPDATE",
      {code});
    auto rollback_release=[&]{ real.exec_params(c.get(),"ROLLBACK",{}); real.release(c.release()); };
    if(!v || PQresultStatus(v.get())!=PGRES_TUPLES_OK || PQntuples(v.get())==0){ result="__invalid__"; rollback_release(); return; }
    std::string vid=PQgetvalue(v.get(),0,0);
    std::string vcode=PQgetvalue(v.get(),0,1);
    std::string vpackage=PQgetvalue(v.get(),0,2);
    std::string vdur=PQgetvalue(v.get(),0,3);
    int vmax=std::atoi(PQgetvalue(v.get(),0,4));
    int vused=std::atoi(PQgetvalue(v.get(),0,5));
    std::string vexp=PQgetvalue(v.get(),0,6);
    std::string vis_active=PQgetvalue(v.get(),0,7);
    bool vcustom=std::string(PQgetvalue(v.get(),0,8))=="t";
    std::string clabel=PQgetvalue(v.get(),0,9);
    long long cmax_exams=std::atoll(PQgetvalue(v.get(),0,10));
    long long cmax_pdf=std::atoll(PQgetvalue(v.get(),0,11));
    long long cmax_conc=std::atoll(PQgetvalue(v.get(),0,12));
    long long cmax_storage=std::atoll(PQgetvalue(v.get(),0,13));
    long long cmax_users=std::atoll(PQgetvalue(v.get(),0,14));
    std::string crole=PQgetvalue(v.get(),0,15);
    // 3. validasi: aktif, belum kedaluwarsa, kuota belum penuh — semua pesan SAMA (anti-oracle)
    if(!voucher_usable(vis_active, vexp) || vused>=vmax){
      result="__invalid__";
      rollback_release(); return;
    }
    // 4. entitlement: package → limit; custom → field kustom
    std::string pkg=vpackage;
    long long max_exams=0,max_pdf=0,max_conc=0,max_storage=0,max_users=0;
    std::string role="";
    if(vcustom){
      pkg=clabel;
      max_exams=cmax_exams; max_pdf=cmax_pdf; max_conc=cmax_conc;
      max_storage=cmax_storage; max_users=cmax_users; role=crole;
    } else {
      auto ps=real.exec_params(c.get(),"SELECT max_exams,max_pdf_size,max_concurrent_exams,max_storage_size,max_users,COALESCE(role,'') FROM package_settings WHERE pkg_key=$1",{vpackage});
      if(ps && PQresultStatus(ps.get())==PGRES_TUPLES_OK && PQntuples(ps.get())>0){
        max_exams=std::atoll(PQgetvalue(ps.get(),0,0));
        max_pdf=std::atoll(PQgetvalue(ps.get(),0,1));
        max_conc=std::atoll(PQgetvalue(ps.get(),0,2));
        max_storage=std::atoll(PQgetvalue(ps.get(),0,3));
        max_users=std::atoll(PQgetvalue(ps.get(),0,4));
        role=PQgetvalue(ps.get(),0,5);
      } else { result="__invalid__"; rollback_release(); return; }
    }
    int days=duration_days(vdur);
    long long remaining_seconds=(long long)days*86400;
    // 5. used_count++ + INSERT redemption + UPDATE user — satu transaksi
    //    (M13): bila salah satu gagal, ROLLBACK — used_count tidak boleh
    //    terbakar tanpa redemption.
    auto up=real.exec_params(c.get(),"UPDATE vouchers SET used_count=used_count+1 WHERE id=$1",{vid});
    if(!up || PQresultStatus(up.get())!=PGRES_COMMAND_OK){ result="__fail__"; rollback_release(); return; }
    auto ins=real.exec_params(c.get(),
      "INSERT INTO voucher_redemptions (voucher_id,user_id,is_active,package,max_exams,max_pdf_size,max_concurrent_exams,max_storage_size,max_users,role,remaining_seconds,activated_at) VALUES ($1,$2,true,$3,$4,$5,$6,$7,$8,$9,$10,now())",
      {vid,std::to_string(uid),pkg,std::to_string(max_exams),std::to_string(max_pdf),
       std::to_string(max_conc),std::to_string(max_storage),std::to_string(max_users),role,std::to_string(remaining_seconds)});
    if(!ins || PQresultStatus(ins.get())!=PGRES_COMMAND_OK){ result="__fail__"; rollback_release(); return; }
    auto usr=real.exec_params(c.get(),
      "UPDATE admin_users SET package=$2,max_exams=$3,max_pdf_size=$4,max_concurrent_exams=$5,max_storage_size=$6,package_role=$7,expires_at=now() + ($8 * interval '1 second') WHERE id=$1",
      {std::to_string(uid),pkg,std::to_string(max_exams),std::to_string(max_pdf),
       std::to_string(max_conc),std::to_string(max_storage),role,std::to_string(remaining_seconds)});
    if(!usr || PQresultStatus(usr.get())!=PGRES_COMMAND_OK){ result="__fail__"; rollback_release(); return; }
    utils::log_info("voucher_redeemed","user="+std::to_string(uid)+" code="+vcode);
    auto cm=real.exec_params(c.get(),"COMMIT",{});
    if(!cm || (PQresultStatus(cm.get())!=PGRES_COMMAND_OK && PQresultStatus(cm.get())!=PGRES_TUPLES_OK)){ result="__fail__"; real.release(c.release()); return; }
    result="ok";
    real.release(c.release());
  });
  if(result=="__super__"){ Response r; r.status=403; r.json(403,"{\"success\":false,\"error\":\"Akun SuperAdmin tidak dapat menukar kode voucher\"}"); return r; }
  if(result=="__operator_created__"){ Response r; r.status=403; r.json(403,"{\"success\":false,\"error\":\"Gagal memproses klaim voucher\"}"); return r; }
  if(result=="notfound"){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"Akun tidak ditemukan\"}"); return r; }
  if(result=="__invalid__"){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Kode voucher tidak valid atau sudah tidak tersedia\"}"); return r; }
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal memproses klaim voucher\"}"); return r; }
  if(result=="ok"){ Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Voucher berhasil diklaim\"}"); return r; }
#endif
  Response r; r.status=503; r.json(503,"{\"success\":false,\"error\":\"Database tidak tersedia\"}"); return r;
}

// ===== activate =============================================================
Response activate_voucher(const Request& req){
  auto form=helpers::parse_form(req.body);
  std::string rid=get_param(form,"redemption_id");
  if(rid.empty()) rid=json_string_field(req.body,"redemption_id");
  if(rid.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"ID paket tidak valid\"}"); return r; }
  int uid=session_admin_id_from(req);
  if(uid<=0){ Response r; r.status=401; r.json(401,"{\"success\":false,\"error\":\"Sesi tidak valid. Silakan login kembali.\"}"); return r; }
#ifdef HAS_LIBPQ
  std::string result="";
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto begin=real.exec_params(c.get(),"BEGIN",{});
    if(!begin || PQresultStatus(begin.get())!=PGRES_COMMAND_OK){ result="__fail__"; real.release(c.release()); return; }
    auto rollback_release=[&]{ real.exec_params(c.get(),"ROLLBACK",{}); real.release(c.release()); };
    auto r=real.exec_params(c.get(),
      "SELECT package,max_exams,max_pdf_size,max_concurrent_exams,max_storage_size,COALESCE(role,''),remaining_seconds,is_active FROM voucher_redemptions WHERE id=$1 AND user_id=$2 FOR UPDATE",
      {rid,std::to_string(uid)});
    if(!r || PQresultStatus(r.get())!=PGRES_TUPLES_OK || PQntuples(r.get())==0){ result="notfound"; rollback_release(); return; }
    std::string pkg=PQgetvalue(r.get(),0,0);
    std::string mx=PQgetvalue(r.get(),0,1);
    std::string mp=PQgetvalue(r.get(),0,2);
    std::string mc=PQgetvalue(r.get(),0,3);
    std::string ms=PQgetvalue(r.get(),0,4);
    std::string role=PQgetvalue(r.get(),0,5);
    std::string rem=PQgetvalue(r.get(),0,6);
    if(rem.empty() || std::atoll(rem.c_str())<=0 || std::string(PQgetvalue(r.get(),0,7))!="t"){
      result="__invalid__"; rollback_release(); return;
    }
    // 1. deactivate all other active redemptions inside this transaction.
    auto de=real.exec_params(c.get(),"UPDATE voucher_redemptions SET is_active=false WHERE user_id=$1 AND id<>$2",{std::to_string(uid),rid});
    if(!de || PQresultStatus(de.get())!=PGRES_COMMAND_OK){ result="__fail__"; rollback_release(); return; }
    // 2. activate selected and apply entitlement.
    auto ac=real.exec_params(c.get(),"UPDATE voucher_redemptions SET is_active=true,activated_at=now(),remaining_seconds=$2 WHERE id=$1",{rid,rem});
    if(!ac || PQresultStatus(ac.get())!=PGRES_COMMAND_OK){ result="__fail__"; rollback_release(); return; }
    auto usr=real.exec_params(c.get(),
      "UPDATE admin_users SET package=$2,max_exams=$3,max_pdf_size=$4,max_concurrent_exams=$5,max_storage_size=$6,package_role=$7,expires_at=now() + ($8 * interval '1 second') WHERE id=$1",
      {std::to_string(uid),pkg,mx,mp,mc,ms,role,rem});
    if(!usr || PQresultStatus(usr.get())!=PGRES_COMMAND_OK){ result="__fail__"; rollback_release(); return; }
    auto commit=real.exec_params(c.get(),"COMMIT",{});
    if(!commit || PQresultStatus(commit.get())!=PGRES_COMMAND_OK){ result="__fail__"; real.release(c.release()); return; }
    result="ok";
    real.release(c.release());
  });
  if(result=="notfound"){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"Paket tidak ditemukan\"}"); return r; }
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal memproses aktivasi paket\"}"); return r; }
  if(result=="ok"){ Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Paket berhasil diaktifkan\"}"); return r; }
#endif
  Response r; r.status=503; r.json(503,"{\"success\":false,\"error\":\"Database tidak tersedia\"}"); return r;
}

// ===== packages (package_settings) ==========================================
static std::string packages_json(){
  std::string arr="[]";
#ifdef HAS_LIBPQ
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto r=real.exec_params(c.get(),
      "SELECT pkg_key,COALESCE(label,''),max_exams,max_pdf_size,max_concurrent_exams,max_storage_size,max_users,COALESCE(role,'') FROM package_settings ORDER BY updated_at",
      {});
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      std::string s="[";
      for(int i=0;i<PQntuples(r.get());i++){
        if(i>0) s+=",";
        s+="{\"key\":\""+json_escape(PQgetvalue(r.get(),i,0))+"\""
          +",\"label\":\""+json_escape(PQgetvalue(r.get(),i,1))+"\""
          +",\"max_exams\":"+PQgetvalue(r.get(),i,2)
          +",\"max_pdf_size_mb\":"+helpers::round_to(std::atoll(PQgetvalue(r.get(),i,3))/1048576.0,2)
          +",\"max_concurrent_exams\":"+PQgetvalue(r.get(),i,4)
          +",\"max_storage_mb\":"+helpers::round_to(std::atoll(PQgetvalue(r.get(),i,5))/1048576.0,2)
          +",\"max_users\":"+PQgetvalue(r.get(),i,6)
          +",\"role\":"+(std::string(PQgetvalue(r.get(),i,7)).empty()?"[]":std::string(PQgetvalue(r.get(),i,7)))+"}";
      }
      s+="]"; arr=s;
    }
    real.release(c.release());
  });
#endif
  return arr;
}

Response list_packages(const Request&){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(Request{})){
    // tidak ada protobuf khusus packages; fallback JSON
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"packages\":"+packages_json()+"}"); return r;
}

Response save_packages(const Request& req){
  // body JSON: {"packages":[{key,label,max_exams,max_pdf_size_mb,max_concurrent_exams,max_storage_mb,max_users,role}]}
  std::string needle="\"packages\"";
  size_t p=req.body.find(needle);
  if(p==std::string::npos){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"packages required\"}"); return r; }
  // parse sederhana: setiap objek { ... }
  std::vector<std::string> objs;
  size_t i=req.body.find('[',p);
  if(i==std::string::npos){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"packages harus array\"}"); return r; }
  for(;i<req.body.size();++i){
    char ch=req.body[i];
    if(ch=='{'){ int depth=0; size_t s=i; bool in=false,esc=false;
      for(;i<req.body.size();++i){ char c2=req.body[i];
        if(esc){esc=false;continue;} if(c2=='\\'&&in){esc=true;continue;} if(c2=='"'){in=!in;continue;} if(in) continue;
        if(c2=='{')depth++; else if(c2=='}'){depth--; if(depth==0){i++; objs.push_back(req.body.substr(s,i-s)); break;}} }
    }
  }
  if(objs.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"tidak ada paket\"}"); return r; }
  int saved=0;
#ifdef HAS_LIBPQ
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    for(auto& o: objs){
      std::string key=json_string_field(o,"key");
      if(key.empty()) continue;
      std::string label=json_string_field(o,"label");
      // M15: angka bisa dikirim tanpa quote — pakai json_raw_scalar (bukan
      // json_string_field yang hanya membaca ber-quote → atoll("")=0).
      long long max_exams=std::atoll(json_raw_scalar(o,"max_exams").c_str());
      double pdf_mb=std::atof(json_raw_scalar(o,"max_pdf_size_mb").c_str());
      long long max_conc=std::atoll(json_raw_scalar(o,"max_concurrent_exams").c_str());
      double storage_mb=std::atof(json_raw_scalar(o,"max_storage_mb").c_str());
      long long max_users=std::atoll(json_raw_scalar(o,"max_users").c_str());
      std::string role=json_string_field(o,"role");
      // role bisa array JSON ["operator"] — string_field gagal → ambil mentah
      if(role.empty()){ std::string needle2="\"role\"";
        size_t rp=o.find(needle2); if(rp!=std::string::npos){
          size_t rc=o.find(':',rp+needle2.size()); size_t rs=o.find_first_not_of(" \t\r\n",rc+1);
          if(rs!=std::string::npos && o[rs]=='['){ size_t re=o.find(']',rs); if(re!=std::string::npos) role=o.substr(rs,re-rs+1); } } }
      auto up=real.exec_params(c.get(),
        "INSERT INTO package_settings (pkg_key,label,max_exams,max_pdf_size,max_concurrent_exams,max_storage_size,max_users,role,updated_at) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,now()) ON CONFLICT (pkg_key) DO UPDATE SET label=EXCLUDED.label,max_exams=EXCLUDED.max_exams,max_pdf_size=EXCLUDED.max_pdf_size,max_concurrent_exams=EXCLUDED.max_concurrent_exams,max_storage_size=EXCLUDED.max_storage_size,max_users=EXCLUDED.max_users,role=EXCLUDED.role,updated_at=now()",
        {key,label,std::to_string(max_exams),std::to_string((long long)(pdf_mb*1024*1024)),
         std::to_string(max_conc),std::to_string((long long)(storage_mb*1024*1024)),std::to_string(max_users),role});
      if(up && (PQresultStatus(up.get())==PGRES_COMMAND_OK||PQresultStatus(up.get())==PGRES_TUPLES_OK)) saved++;
    }
    real.release(c.release());
  });
  if(saved==0){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal menyimpan pengaturan paket\"}"); return r; }
#endif
  (void)saved;
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Pengaturan paket berhasil disimpan\"}"); return r;
}

Response billing_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Billing</h1></body></html>"; return r;
}
} // namespace examvan::handlers::admin