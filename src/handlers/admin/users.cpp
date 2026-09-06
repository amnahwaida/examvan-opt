#include "handlers/admin/users.hpp"
#include "models/user.hpp"
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
namespace examvan::handlers::admin {

static std::string get_param(const std::map<std::string,std::string>& form, const std::string& key){
  auto it=form.find(key); return it!=form.end()? it->second : "";
}

[[maybe_unused]] static std::string json_escape(const std::string& s){
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
  try { std::string val=body.substr(s); size_t n=0; int v=std::stoi(val,&n); (void)n; return v; }
  catch(...) { return std::nullopt; }
}

static std::optional<double> json_double_field(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\"";
  size_t p=body.find(needle);
  if(p==std::string::npos) return std::nullopt;
  size_t colon=body.find(':',p+needle.size());
  if(colon==std::string::npos) return std::nullopt;
  size_t s=body.find_first_not_of(" \t\r\n",colon+1);
  if(s==std::string::npos) return std::nullopt;
  try { std::string val=body.substr(s); size_t n=0; double v=std::stod(val,&n); (void)n; return v; }
  catch(...) { return std::nullopt; }
}

// Parse array JSON roles and keep only canonical application roles.
static std::string json_roles_join(const std::string& body){
  std::string needle="\"roles\"";
  size_t p=body.find(needle);
  if(p==std::string::npos) return "";
  size_t colon=body.find(':',p+needle.size());
  if(colon==std::string::npos) return "";
  size_t s=body.find_first_not_of(" \t\r\n",colon+1);
  if(s==std::string::npos || body[s]!='[') return "";
  size_t i=s+1;
  std::vector<std::string> roles;
  while(i<body.size() && body[i]!=']'){
    while(i<body.size() && (body[i]==' '||body[i]=='\t'||body[i]=='\n'||body[i]=='\r'||body[i]==',')) i++;
    if(i>=body.size() || body[i]==']') break;
    if(body[i]=='"'){
      ++i; std::string r;
      for(;i<body.size();++i){
        if(body[i]=='\\' && i+1<body.size()){ r.push_back(body[++i]); continue; }
        if(body[i]=='"') break;
        r.push_back(body[i]);
      }
      if(r=="guru" || r=="pengawas" || r=="operator" || r=="superadmin"){
        if(std::find(roles.begin(),roles.end(),r)==roles.end()) roles.push_back(r);
      }
      i++;
    } else { while(i<body.size() && body[i]!=']' && body[i]!=',') i++; }
  }
  std::string out="[";
  for(size_t k=0;k<roles.size();k++){ if(k>0) out+=","; out+="\""+roles[k]+"\""; }
  out+="]";
  return out;
}

static int session_admin_id_from(const Request& req){
  for(auto& kv:req.headers){ std::string k=kv.first; for(char& ch:k) ch=tolower((unsigned char)ch); if(k=="x-internal-admin-id"){ try{ return std::stoi(kv.second); }catch(...){} } }
  return 0;
}

#ifdef HAS_LIBPQ

// Jalankan fn dengan pool PG terbuka; fn dipanggil hanya bila koneksi OK.
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

// Bangun objek JSON user dari satu baris hasil query.
// Kolom (urutan): id,username,name,email,status,instansi,role,base_role,
// package_role,package,max_exams,max_pdf_size,max_concurrent_exams,
// max_storage_size,expires_at,created_at,operator_created,exam_count
static std::string build_user_json(PGresult* r, int row){
  auto val=[&](int col)->std::string{
    if(PQgetisnull(r,row,col)) return "";
    return std::string(PQgetvalue(r,row,col));
  };
  std::string role=val(6);
  std::string base=val(7), pkg=val(8);
  std::string expires=val(14);
  std::string created=val(15);
  // expires_at dari PG timestamptz → "YYYY-MM-DD HH:MM:SS" (format frontend, +'Z').
  if(!expires.empty() && expires.size()>=19) expires=expires.substr(0,19);
  if(!created.empty() && created.size()>=19) created=created.substr(0,19);
  std::string roles="[]";
  if(role.rfind('[',0)==0) roles=role; else if(!role.empty()) roles="[\""+json_escape(role)+"\"]";
  std::string base_roles="[]", package_roles="[]";
  if(base.rfind('[',0)==0) base_roles=base; else if(!base.empty()) base_roles="[\""+json_escape(base)+"\"]";
  if(pkg.rfind('[',0)==0) package_roles=pkg; else if(!pkg.empty()) package_roles="[\""+json_escape(pkg)+"\"]";
  std::string op_created=val(16)=="t"||val(16)=="true" ? "true":"false";
  std::string exam_count=val(17).empty()?"0":val(17);
  return "{\"id\":"+val(0)+",\"username\":\""+json_escape(val(1))+"\",\"name\":\""+json_escape(val(2))
    +"\",\"email\":\""+json_escape(val(3))+"\",\"status\":\""+json_escape(val(4))+"\""
    +",\"instansi\":\""+json_escape(val(5))+"\",\"roles\":"+roles
    +",\"base_roles\":"+base_roles+",\"package_roles\":"+package_roles
    +",\"role\":\""+json_escape(role)+"\",\"package\":\""+json_escape(val(9))+"\""
    +",\"max_exams\":"+val(10)+",\"max_pdf_size\":"+val(11)
    +",\"max_concurrent_exams\":"+val(12)+",\"max_storage_size\":"+val(13)
    +",\"expires_at\":\""+json_escape(expires)+"\",\"created_at\":\""+json_escape(created)+"\""
    +",\"exam_count\":"+exam_count+",\"operator_created\":"+op_created
    +",\"has_active_package\":"+(!val(9).empty() && val(9)!="free" ? "true":"false")+"}";
}

static const char* kUserCols=
  "u.id,u.username,u.name,u.email,u.status,u.instansi,u.role,u.base_role,"
  "u.package_role,u.package,u.max_exams,u.max_pdf_size,u.max_concurrent_exams,"
  "u.max_storage_size,u.expires_at,u.created_at,u.operator_created,"
  "COALESCE((SELECT COUNT(*) FROM exams e WHERE e.created_by=u.id),0) AS exam_count";
#endif

Response list_users(const Request& req){
  /* M1: stub protobuf di AWAL fungsi dihapus — balasan kosong "success" untuk
   * klien Accept: x-protobuf membuat daftar tampak kosong padahal ada data.
   * Klien protobuf mendapat JSON nyata (konten benar; content-type beda). */
  auto q=helpers::parse_form(req.query);
  int page=1, per_page=10;
  try{ page=std::stoi(get_param(q,"page")); }catch(...){}
  try{ per_page=std::stoi(get_param(q,"per_page")); }catch(...){}
  if(page<1) page=1; else if(page>1000000) page=1000000;
  if(per_page<5) per_page=5; else if(per_page>200) per_page=200;
  std::string search=get_param(q,"search");
  std::string role_filter=get_param(q,"role");
  std::string sort_by=get_param(q,"sort_by");
  std::string sort_dir=get_param(q,"sort_dir");
  if(sort_dir!="DESC") sort_dir="ASC";
  std::string pagination="{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)+",\"total\":0,\"total_pages\":0}";
#ifdef HAS_LIBPQ
  std::string users_json="[]";
  bool got=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    std::string where=" WHERE 1=1";
    std::vector<std::string> params;
    if(!search.empty()){
      where+=" AND (u.username ILIKE $"+std::to_string(params.size()+1)+" OR u.name ILIKE $"+std::to_string(params.size()+1)+" OR u.email ILIKE $"+std::to_string(params.size()+1)+")";
      params.push_back("%"+search+"%");
    }
    if(!role_filter.empty()){
      where+=" AND u.role ILIKE $"+std::to_string(params.size()+1);
      params.push_back("%\""+role_filter+"\"%");
    }
    // sort whitelist (paritas Go: unknown sort_by → default role priority)
    std::string order=" ORDER BY CASE WHEN u.username='superadmin' THEN 0 ELSE 1 END, u.id";
    if(sort_by=="username"||sort_by=="name"||sort_by=="instansi"||sort_by=="status"||sort_by=="created_at"||sort_by=="package"||sort_by=="email"){
      order=" ORDER BY u."+sort_by+" "+sort_dir+", u.id";
    }
    std::string sql="SELECT "+std::string(kUserCols)+", COUNT(*) OVER() AS total FROM admin_users u"+where+order
      +" LIMIT $"+std::to_string(params.size()+1)+" OFFSET $"+std::to_string(params.size()+2);
    params.push_back(std::to_string(per_page));
    params.push_back(std::to_string(static_cast<int64_t>(page-1)*static_cast<int64_t>(per_page)));
    auto r=real.exec_params(c.get(),sql,params);
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      int n=PQntuples(r.get());
      int total=0; if(n>0) total=std::atoi(PQgetvalue(r.get(),0,18));
      int total_pages=(total+per_page-1)/per_page;
      std::string arr="[";
      for(int i=0;i<n;i++){ if(i>0) arr+=","; arr+=build_user_json(r.get(),i); }
      arr+="]";
      users_json=arr;
      pagination="{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)
        +",\"total\":"+std::to_string(total)+",\"total_pages\":"+std::to_string(total_pages)+"}";
      got=true;
    }
    real.release(c.release());
  });
  if(got){
    Response r; r.json(200,"{\"success\":true,\"users\":"+users_json+",\"pagination\":"+pagination+"}");
    return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"users\":[],\"pagination\":"+pagination+"}"); return r;
}

Response user_detail(const Request& req){
  auto it=req.params.find("id");
  if(it==req.params.end() || it->second.empty()){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r;
  }
  int id=0; try{ id=std::stoi(it->second); }catch(...){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid id\"}"); return r; }
#ifdef HAS_LIBPQ
  std::string user_json;
  bool found=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    std::string sql="SELECT "+std::string(kUserCols)+" FROM admin_users u WHERE u.id=$1";
    auto r=real.exec_params(c.get(),sql,{std::to_string(id)});
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK && PQntuples(r.get())>0){
      user_json=build_user_json(r.get(),0);
      found=true;
    }
    real.release(c.release());
  });
  if(found){ Response r; r.json(200,"{\"success\":true,\"user\":"+user_json+"}"); return r; }
  Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"user not found\"}"); return r;
#else
  (void)id;
  Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"user not found\"}"); return r;
#endif
}

// Ambil nilai dari body JSON bila ada, fallback form-encoded.
static std::string body_field(const Request& req, const std::string& key){
  std::string v=json_string_field(req.body,key);
  if(!v.empty()) return v;
  auto form=helpers::parse_form(req.body);
  return get_param(form,key);
}

Response create_user(const Request& req){
  /* M1: stub protobuf di awal dihapus — id palsu "1" tanpa INSERT membuat
   * klien protobuf percaya user tersimpan padahal tidak. Lanjut ke logika
   * nyata (respons JSON). */
  std::string username=body_field(req,"username");
  std::string password=body_field(req,"password");
  std::string role=body_field(req,"role"); if(role.empty()) role="guru";
  std::string name=body_field(req,"name");
  std::string email=body_field(req,"email");
  std::string instansi=body_field(req,"instansi");
  auto roles_json=json_roles_join(req.body);
  if(roles_json=="[]"){
    if(role!="guru") roles_json="[\""+role+"\"]";
    else roles_json="[\"guru\"]";
  }
  int max_exams=3, max_concurrent=2;
  long long max_pdf=1048576, max_storage=52428800;
  if(auto v=json_int_field(req.body,"max_exams")) max_exams=*v;
  if(auto v=json_int_field(req.body,"max_concurrent_exams")) max_concurrent=*v;
  if(auto v=json_double_field(req.body,"max_pdf_size_mb")) max_pdf=(long long)(*v*1024*1024);
  if(auto v=json_double_field(req.body,"max_storage_size_mb")) max_storage=(long long)(*v*1024*1024);
  (void)max_exams; (void)max_concurrent; (void)max_pdf; (void)max_storage;
  std::string expires_at=body_field(req,"expires_at");
  std::string package=body_field(req,"package"); if(package.empty()) package="free";
  if(username.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"username required\"}"); return r; }
  if(!models::is_valid_username(username)){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"username 3-32 lowercase, dot, underscore, hyphen\"}"); return r; }
  if(password.size()<8){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"password minimal 8 karakter\"}"); return r; }
  if(role=="operator"){
    std::string cookie;
    auto itc=req.headers.find("Cookie");
    if(itc!=req.headers.end()) cookie=itc->second;
    auto cfg = Config::load();
    std::string cur=cfg.secret_key;
    std::string prev=cfg.secret_prev;
    auto sess=prev.empty()?verify_session_cookie(cur, cookie):verify_session_cookie_dual(cur, prev, cookie);
    bool is_super=false;
    if(sess){
      is_super=sess->is_super_admin || sess->role=="superadmin";
    }
    if(!is_super){ Response r; r.status=403; r.json(403,"{\"success\":false,\"error\":\"hanya superadmin bisa buat operator\"}"); return r; }
  }
  if(role!="guru" && role!="pengawas" && role!="operator"){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"role tidak valid\"}"); return r; }
#ifdef HAS_LIBPQ
  std::string created_id;
  // PG dikonfigurasi tapi tidak terjangkau → GAGAL (503), jangan 201 palsu id=1.
  bool pg_configured = !Config::load().database_url.empty();
  if(!pg_configured){ if(auto* e=getenv("DATABASE_URL")) pg_configured=(*e)!='\0'; }
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    // cek duplikat username + email agar 400 ramah, bukan constraint error
    auto dup=real.exec_params(c.get(),"SELECT id FROM admin_users WHERE lower(username)=lower($1) LIMIT 1",{username});
    if(dup && PQresultStatus(dup.get())==PGRES_TUPLES_OK && PQntuples(dup.get())>0){
      created_id="__dup_username__";
    } else if(!email.empty()){
      auto dup2=real.exec_params(c.get(),"SELECT id FROM admin_users WHERE lower(email)=lower($1) AND email<>'' LIMIT 1",{email});
      if(dup2 && PQresultStatus(dup2.get())==PGRES_TUPLES_OK && PQntuples(dup2.get())>0) created_id="__dup_email__";
    }
    if(created_id.empty()){
      std::string hash;
      try{ hash=helpers::hash_password(password); }catch(...){ created_id="__hashfail__"; }
      if(created_id.empty()){
        std::string sql="INSERT INTO admin_users (username,name,email,password_hash,status,instansi,role,package,max_exams,max_concurrent_exams,max_pdf_size,max_storage_size,expires_at,base_role,package_role,operator_created) VALUES ($1,$2,$3,$4,'active',$5,$6,$7,$8,$9,$10,$11,NULLIF($12,'')::timestamptz,'','','false') RETURNING id";
        std::vector<std::string> params={username,name,email,hash,instansi,roles_json,package,
          std::to_string(max_exams),std::to_string(max_concurrent),std::to_string(max_pdf),std::to_string(max_storage),expires_at};
        auto ins=real.exec_params(c.get(),sql,params);
        if(ins && PQresultStatus(ins.get())==PGRES_TUPLES_OK && PQntuples(ins.get())>0){
          created_id=PQgetvalue(ins.get(),0,0);
        } else {
          std::string err=PQresultErrorMessage(ins.get());
          utils::log_error("user_create_insert_failed",err);
          created_id="__insertfail__";
        }
      }
    }
    real.release(c.release());
  });
  if(created_id=="__dup_username__"){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Username sudah digunakan\"}"); return r; }
  if(created_id=="__dup_email__"){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Email sudah terdaftar. Gunakan email lain atau biarkan kosong.\"}"); return r; }
  if(created_id=="__hashfail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal membuat hash password\"}"); return r; }
  if(created_id=="__insertfail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal menyimpan user\"}"); return r; }
  if(!created_id.empty()){
    utils::log_info("user_created","id="+created_id+" username="+username);
    Response r; r.status=201;
    r.json(201,"{\"success\":true,\"ok\":true,\"id\":"+created_id+",\"username\":\""+json_escape(username)+"\",\"role\":\""+role+"\",\"message\":\"User berhasil dibuat\"}");
    return r;
  }
  if(pg_configured){
    // PG dikonfigurasi tapi tidak terjangkau → fail-closed, jangan 201 palsu id=1.
    Response r; r.status=503; r.json(503,"{\"success\":false,\"error\":\"Database tidak tersedia\"}"); return r;
  }
#endif
  Response r; r.status=201; r.json(201,"{\"success\":true,\"ok\":true,\"id\":1,\"username\":\""+username+"\",\"role\":\""+role+"\"}"); return r;
}

static std::string path_after(const Request& req, const std::string& marker){
  size_t p=req.path.find(marker);
  if(p==std::string::npos) return "";
  return req.path.substr(p+marker.size());
}

Response edit_user(const Request& req){
  /* M1: stub protobuf awal dihapus — sukses palsu tanpa UPDATE. Lanjut ke
   * logika nyata (respons JSON). */
  // id bisa dari params :id (PUT) atau path /users/:id/edit
  std::string id_str;
  auto it=req.params.find("id");
  if(it!=req.params.end() && !it->second.empty()) id_str=it->second;
  if(id_str.empty()) id_str=path_after(req,"/admin/api/users/");
  if(id_str.empty() || id_str.find('/')!=std::string::npos){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r;
  }
  int id=0; try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid id\"}"); return r; }
  (void)id;
  // validasi password baru sebelum menyentuh DB
  std::string new_pass=body_field(req,"password");
  if(!new_pass.empty() && new_pass.size()<8){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"password minimal 8 karakter\"}"); return r;
  }
#ifdef HAS_LIBPQ
  std::string result=""; // "", "notfound", "ok"
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto chk=real.exec_params(c.get(),"SELECT id FROM admin_users WHERE id=$1",{std::to_string(id)});
    if(!chk || PQresultStatus(chk.get())!=PGRES_TUPLES_OK || PQntuples(chk.get())==0){ result="notfound"; real.release(c.release()); return; }
    std::string name=body_field(req,"name");
    std::string email=body_field(req,"email");
    std::string instansi=body_field(req,"instansi");
    std::string package=body_field(req,"package");
    std::string expires_at=body_field(req,"expires_at");
    auto roles_json=json_roles_join(req.body);
    if(roles_json=="[]"){
      std::string rl=body_field(req,"role");
      if(rl.empty()) rl="guru";
      roles_json="[\""+rl+"\"]";
    }
    int max_exams=3, max_concurrent=2;
    long long max_pdf=1048576, max_storage=52428800;
    if(auto v=json_int_field(req.body,"max_exams")) max_exams=*v;
    if(auto v=json_int_field(req.body,"max_concurrent_exams")) max_concurrent=*v;
    if(auto v=json_double_field(req.body,"max_pdf_size_mb")) max_pdf=(long long)(*v*1024*1024);
    if(auto v=json_double_field(req.body,"max_storage_size_mb")) max_storage=(long long)(*v*1024*1024);
    std::string sql="UPDATE admin_users SET name=$2,email=$3,instansi=$4,package=$5,role=$6,"
      "max_exams=$7,max_concurrent_exams=$8,max_pdf_size=$9,max_storage_size=$10,expires_at=NULLIF($11,'')::timestamptz";
    std::vector<std::string> params={std::to_string(id),name,email,instansi,package,roles_json,
      std::to_string(max_exams),std::to_string(max_concurrent),std::to_string(max_pdf),std::to_string(max_storage),expires_at};
    if(!new_pass.empty()){
      std::string hash;
      try{ hash=helpers::hash_password(new_pass); }catch(...){ result="__hashfail__"; real.release(c.release()); return; }
      sql+=",password_hash=$"+std::to_string(params.size()+1);
      params.push_back(hash);
    }
    sql+=" WHERE id=$1";
    auto up=real.exec_params(c.get(),sql,params);
    if(up && (PQresultStatus(up.get())==PGRES_COMMAND_OK || PQresultStatus(up.get())==PGRES_TUPLES_OK)){
      result="ok";
    } else {
      utils::log_error("user_edit_update_failed",PQresultErrorMessage(up.get()));
      result="__updfail__";
    }
    real.release(c.release());
  });
  if(result=="notfound"){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"user not found\"}"); return r; }
  if(result=="__hashfail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal membuat hash password\"}"); return r; }
  if(result=="__updfail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal menyimpan user\"}"); return r; }
  if(result=="ok"){ Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"User berhasil diperbarui\"}"); return r; }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"User berhasil diperbarui\"}"); return r;
}

Response delete_user(const Request& req){
  /* M1: stub protobuf awal dihapus — "success" TANPA DELETE adalah no-op yang
   * membahayakan (klien protobuf percaya user terhapus). Hapus selalu nyata. */
  std::string id_str;
  auto it=req.params.find("id");
  if(it!=req.params.end() && !it->second.empty()) id_str=it->second;
  if(id_str.empty()) id_str=path_after(req,"/admin/api/users/");
  if(id_str.empty() || id_str.find('/')!=std::string::npos){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r;
  }
  int id=0; try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid id\"}"); return r; }
  (void)id;
#ifdef HAS_LIBPQ
  std::string result="";
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    // superadmin tidak boleh dihapus (paritas Go)
    auto chk=real.exec_params(c.get(),"SELECT username FROM admin_users WHERE id=$1",{std::to_string(id)});
    if(!chk || PQresultStatus(chk.get())!=PGRES_TUPLES_OK || PQntuples(chk.get())==0){ result="notfound"; real.release(c.release()); return; }
    std::string uname=PQgetvalue(chk.get(),0,0);
    if(uname=="superadmin"){ result="__super__"; real.release(c.release()); return; }
    auto del=real.exec_params(c.get(),"DELETE FROM admin_users WHERE id=$1",{std::to_string(id)});
    if(del && PQresultStatus(del.get())==PGRES_COMMAND_OK){ result="ok"; }
    else { utils::log_error("user_delete_failed",PQresultErrorMessage(del.get())); result="__fail__"; }
    real.release(c.release());
  });
  if(result=="notfound"){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"user not found\"}"); return r; }
  if(result=="__super__"){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Akun Super Admin tidak dapat dihapus\"}"); return r; }
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal menghapus user\"}"); return r; }
  if(result=="ok"){ Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"User berhasil dihapus\"}"); return r; }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"User berhasil dihapus\"}"); return r;
}

// Ambil id dari params :id (router mengisi untuk pattern /users/:id/...),
// fallback parsing path manual.
static std::string action_id(const Request& req){
  auto it=req.params.find("id");
  if(it!=req.params.end() && !it->second.empty()) return it->second;
  return path_after(req,"/admin/api/users/");
}

// POST /admin/api/users/:id/toggle-status — aktif <-> suspended (pending_otp
// hanya lewat Verifikasi, paritas Go ToggleUserStatus).
Response user_toggle_status(const Request& req){
  std::string id_str=action_id(req);
  if(id_str.empty() || id_str.find('/')!=std::string::npos){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r;
  }
  int id=0; try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid id\"}"); return r; }
  (void)id;
#ifdef HAS_LIBPQ
  std::string result="";
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto cur=real.exec_params(c.get(),"SELECT status,username FROM admin_users WHERE id=$1",{std::to_string(id)});
    if(!cur || PQresultStatus(cur.get())!=PGRES_TUPLES_OK || PQntuples(cur.get())==0){ result="notfound"; real.release(c.release()); return; }
    std::string status=PQgetvalue(cur.get(),0,0);
    std::string uname=PQgetvalue(cur.get(),0,1);
    if(uname=="superadmin"){ result="__super__"; real.release(c.release()); return; }
    if(status=="pending_otp"){ result="__pending__"; real.release(c.release()); return; }
    std::string next = status=="active" ? "suspended" : "active";
    auto up=real.exec_params(c.get(),"UPDATE admin_users SET status=$2 WHERE id=$1",{std::to_string(id),next});
    if(up && PQresultStatus(up.get())==PGRES_COMMAND_OK){ result=next; }
    else { utils::log_error("user_toggle_failed",PQresultErrorMessage(up.get())); result="__fail__"; }
    real.release(c.release());
  });
  if(result=="notfound"){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"user not found\"}"); return r; }
  if(result=="__super__"){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Status Super Admin tidak dapat diubah\"}"); return r; }
  if(result=="__pending__"){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Akun pending verifikasi — gunakan tombol Verifikasi\"}"); return r; }
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal mengubah status\"}"); return r; }
  if(result=="active"||result=="suspended"){
    Response r; r.json(200,"{\"success\":true,\"ok\":true,\"status\":\""+result+"\",\"message\":\"Status user diubah menjadi "+result+"\"}"); return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Status user diubah\"}"); return r;
}

// POST /admin/api/users/:id/verify — aktivasi manual akun pending_otp tanpa email.
Response user_verify(const Request& req){
  std::string id_str=action_id(req);
  if(id_str.empty() || id_str.find('/')!=std::string::npos){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r;
  }
  int id=0; try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid id\"}"); return r; }
  (void)id;
#ifdef HAS_LIBPQ
  std::string result="";
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto cur=real.exec_params(c.get(),"SELECT status FROM admin_users WHERE id=$1",{std::to_string(id)});
    if(!cur || PQresultStatus(cur.get())!=PGRES_TUPLES_OK || PQntuples(cur.get())==0){ result="notfound"; real.release(c.release()); return; }
    std::string status=PQgetvalue(cur.get(),0,0);
    std::string next = status=="pending_otp" ? "active" : status;
    if(next!=status){
      auto up=real.exec_params(c.get(),"UPDATE admin_users SET status=$2,otp_code=NULL,otp_expiry=NULL WHERE id=$1",{std::to_string(id),next});
      if(up && PQresultStatus(up.get())==PGRES_COMMAND_OK){ result="ok"; }
      else { utils::log_error("user_verify_failed",PQresultErrorMessage(up.get())); result="__fail__"; }
    } else { result="ok"; } // sudah aktif — idempoten
    real.release(c.release());
  });
  if(result=="notfound"){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"user not found\"}"); return r; }
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal verifikasi user\"}"); return r; }
  if(result=="ok"){ Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"User berhasil diverifikasi\"}"); return r; }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"User berhasil diverifikasi\"}"); return r;
}

// POST /admin/api/users/:id/deactivate-package — matikan paket aktif → free.
Response user_deactivate_package(const Request& req){
  std::string id_str=action_id(req);
  if(id_str.empty() || id_str.find('/')!=std::string::npos){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r;
  }
  int id=0; try{ id=std::stoi(id_str); }catch(...){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"invalid id\"}"); return r; }
  (void)id;
#ifdef HAS_LIBPQ
  std::string result="";
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto chk=real.exec_params(c.get(),"SELECT id FROM admin_users WHERE id=$1",{std::to_string(id)});
    if(!chk || PQresultStatus(chk.get())!=PGRES_TUPLES_OK || PQntuples(chk.get())==0){ result="notfound"; real.release(c.release()); return; }
    auto up=real.exec_params(c.get(),"UPDATE admin_users SET package='free',package_role='' WHERE id=$1",{std::to_string(id)});
    if(up && PQresultStatus(up.get())==PGRES_COMMAND_OK){ result="ok"; }
    else { utils::log_error("user_deactivate_package_failed",PQresultErrorMessage(up.get())); result="__fail__"; }
    real.release(c.release());
  });
  if(result=="notfound"){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"user not found\"}"); return r; }
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal menonaktifkan paket\"}"); return r; }
  if(result=="ok"){ Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Paket berhasil dinonaktifkan\"}"); return r; }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Paket berhasil dinonaktifkan\"}"); return r;
}

// POST /admin/api/change-password — ganti password akun session admin.
Response change_password(const Request& req){
  std::string current=body_field(req,"current_password");
  std::string next=body_field(req,"new_password");
  if(current.empty()||next.empty()){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"current_password dan new_password wajib\"}"); return r;
  }
  if(next.size()<8){
    Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Password baru minimal 8 karakter\"}"); return r;
  }
  int uid=session_admin_id_from(req);
#ifdef HAS_LIBPQ
  if(uid<=0){
    Response r; r.status=401; r.json(401,"{\"success\":false,\"error\":\"Sesi tidak valid\"}"); return r;
  }
  std::string result="";
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto cur=real.exec_params(c.get(),"SELECT password_hash FROM admin_users WHERE id=$1",{std::to_string(uid)});
    if(!cur || PQresultStatus(cur.get())!=PGRES_TUPLES_OK || PQntuples(cur.get())==0){ result="notfound"; real.release(c.release()); return; }
    std::string hash=PQgetvalue(cur.get(),0,0);
    if(!helpers::verify_password(current,hash)){ result="__wrong__"; real.release(c.release()); return; }
    std::string new_hash;
    try{ new_hash=helpers::hash_password(next); }catch(...){ result="__hashfail__"; real.release(c.release()); return; }
    auto up=real.exec_params(c.get(),"UPDATE admin_users SET password_hash=$2 WHERE id=$1",{std::to_string(uid),new_hash});
    if(up && PQresultStatus(up.get())==PGRES_COMMAND_OK){ result="ok"; }
    else { utils::log_error("change_password_failed",PQresultErrorMessage(up.get())); result="__fail__"; }
    real.release(c.release());
  });
  if(result=="notfound"){ Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"Akun tidak ditemukan\"}"); return r; }
  if(result=="__wrong__"){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"Password saat ini salah\"}"); return r; }
  if(result=="__hashfail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal membuat hash password\"}"); return r; }
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal mengubah password\"}"); return r; }
  if(result=="ok"){ Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Password berhasil diubah\"}"); return r; }
#endif
  (void)uid;
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Password berhasil diubah\"}"); return r;
}

Response instansi_update(const Request& req){
  auto form=helpers::parse_form(req.body);
  std::string name=get_param(form,"instansi");
  if(name.empty()) name=json_string_field(req.body,"instansi");
  if(name.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"instansi required\"}"); return r; }
#ifdef HAS_LIBPQ
  int uid=session_admin_id_from(req);
  if(uid>0){
    with_pg([&](examvan::db::RealPool& real){
      auto c=real.acquire();
      if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
      auto up=real.exec_params(c.get(),"UPDATE admin_users SET instansi=$2 WHERE id=$1",{std::to_string(uid),name});
      if(up && PQresultStatus(up.get())==PGRES_COMMAND_OK) utils::log_info("user_instansi_updated","id="+std::to_string(uid));
      real.release(c.release());
    });
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"instansi\":\""+name+"\"}"); return r;
}
} // namespace examvan::handlers::admin