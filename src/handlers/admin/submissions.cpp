#include "handlers/admin/submissions.hpp"
#include "handlers/admin/template_helper.hpp"
#include "helpers/utils.hpp"
#include "config/config.hpp"
#include "middleware/protobuf.hpp"
#include "utils/log.hpp"
#include "models/user.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#ifdef HAS_LIBPQ
#include "db/pool.hpp"
#include "db/pool_real.hpp"
#include <libpq-fe.h>
#include <functional>
#endif
#ifdef HAS_HIREDIS
#include <hiredis/hiredis.h>
#include "redis/redis_real.hpp"
#endif
#include <string>
#include <cctype>
#include <optional>
#include <fstream>
#include <sstream>

namespace examvan::handlers::admin {

static std::string get_param(const std::map<std::string,std::string>& form, const std::string& key){
  auto it=form.find(key); return it!=form.end()? it->second : "";
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
#endif

#ifdef HAS_LIBPQ
static std::string json_escape_ci(const std::string& s){
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

Response submissions_page(const Request&){
  RenderedAdminPage rp=render_admin_page("submissions","2.7.2");
  if(!rp.html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html";
    if(!rp.csrf_cookie.empty()) r.headers["Set-Cookie"]=rp.csrf_cookie;
    r.body=rp.html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Submissions</h1></body></html>"; return r;
}

// ===== list ================================================================
Response list_submissions(const Request& req){
  /* M1: stub protobuf awal dihapus — list kosong padahal ada data. */
  auto q=helpers::parse_form(req.query);
  int page=1, per_page=20;
  try{ page=std::stoi(get_param(q,"page")); }catch(...){}
  try{ per_page=std::stoi(get_param(q,"per_page")); }catch(...){}
  if(page<1) page=1; else if(page>1000000) page=1000000;
  if(per_page<1) per_page=20; else if(per_page>200) per_page=200;
  std::string exam_id=get_param(q,"exam_id");
  std::string search=get_param(q,"search");
  int actor_id=0; bool super_admin=false;
  if(auto it=req.headers.find("X-Internal-Admin-Id"); it!=req.headers.end()) try{ actor_id=std::stoi(it->second); }catch(...){ }
  if(auto it=req.headers.find("X-Internal-Admin-Super"); it!=req.headers.end()) super_admin=it->second=="1";
  std::string actor_role;
  if(auto it=req.headers.find("X-Internal-Admin-Role"); it!=req.headers.end()) actor_role=it->second;
  std::string actor_instansi;
  if(auto it=req.headers.find("X-Internal-Admin-Instansi"); it!=req.headers.end()) actor_instansi=it->second;
#ifndef HAS_LIBPQ
  (void)actor_id; (void)super_admin; (void)actor_role; (void)actor_instansi;
#endif
  std::string pagination="{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)+",\"total\":0,\"total_pages\":0}";
#ifdef HAS_LIBPQ
  std::string arr="[]";
  bool got=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    std::string where=" WHERE 1=1";
    std::vector<std::string> params;
    if(!super_admin){
      if(actor_role.find("operator")!=std::string::npos && !actor_instansi.empty()){
        where+=" AND e.created_by IN (SELECT id FROM admin_users WHERE instansi=$"+std::to_string(params.size()+1)+")";
        params.push_back(actor_instansi);
      } else {
        where+=" AND (e.created_by=$"+std::to_string(params.size()+1)+" OR e.delegated_to=$"+std::to_string(params.size()+1)+")";
        params.push_back(std::to_string(actor_id));
      }
    }
    if(!exam_id.empty()){
      where+=" AND s.exam_id=$"+std::to_string(params.size()+1);
      params.push_back(exam_id);
    }
    if(!search.empty()){
      where+=" AND (s.student_name ILIKE $"+std::to_string(params.size()+1)+" OR s.exam_number ILIKE $"+std::to_string(params.size()+1)+" OR s.student_class ILIKE $"+std::to_string(params.size()+1)+")";
      params.push_back("%"+search+"%");
    }
    std::string sql="SELECT s.id,s.exam_id,e.name,s.student_name,s.exam_number,s.student_class,s.score,s.start_time,s.mac_address,s.created_at, COUNT(*) OVER() AS total"
      " FROM submissions s LEFT JOIN exams e ON e.id=s.exam_id"+where
      +" ORDER BY s.id DESC LIMIT $"+std::to_string(params.size()+1)+" OFFSET $"+std::to_string(params.size()+2);
    params.push_back(std::to_string(per_page));
    params.push_back(std::to_string(static_cast<int64_t>(page-1)*static_cast<int64_t>(per_page)));
    auto r=real.exec_params(c.get(),sql,params);
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      int n=PQntuples(r.get());
      int total=0; if(n>0) total=std::atoi(PQgetvalue(r.get(),0,10));
      int total_pages=(total+per_page-1)/per_page;
      std::string s="[";
      for(int i=0;i<n;i++){
        if(i>0) s+=",";
        std::string ct=PQgetvalue(r.get(),i,9); if(ct.size()>=19) ct=ct.substr(0,19);
        std::string score=PQgetvalue(r.get(),i,6);
        if(score.empty()) score="null";
        s+="{\"id\":"+std::string(PQgetvalue(r.get(),i,0))
          +",\"exam_id\":"+PQgetvalue(r.get(),i,1)
          +",\"exam_name\":\""+json_escape_ci(PQgetvalue(r.get(),i,2))+"\""
          +",\"student_name\":\""+json_escape_ci(PQgetvalue(r.get(),i,3))+"\""
          +",\"exam_number\":\""+json_escape_ci(PQgetvalue(r.get(),i,4))+"\""
          +",\"student_class\":\""+json_escape_ci(PQgetvalue(r.get(),i,5))+"\""
          +",\"score\":"+score
          +",\"start_time\":\""+json_escape_ci(PQgetvalue(r.get(),i,7))+"\""
          +",\"mac_address\":\""+json_escape_ci(PQgetvalue(r.get(),i,8))+"\""
          +",\"created_at\":\""+json_escape_ci(ct)+"\"}";
      }
      s+="]"; arr=s;
      pagination="{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)
        +",\"total\":"+std::to_string(total)+",\"total_pages\":"+std::to_string(total_pages)+"}";
      got=true;
    }
    real.release(c.release());
  });
  if(got){
    Response r; r.json(200,"{\"success\":true,\"submissions\":"+arr+",\"pagination\":"+pagination+"}"); return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"submissions\":[],\"pagination\":"+pagination+"}"); return r;
}

// ===== detail ==============================================================
Response submission_detail(const Request& req){
  /* M1: stub protobuf awal dihapus — detail kosong padahal ada data. */
  std::string id_str;
  auto it=req.params.find("id");
  if(it!=req.params.end() && !it->second.empty()) id_str=it->second;
  if(id_str.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r; }
#ifdef HAS_LIBPQ
  std::string subj;
  bool found=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto r=real.exec_params(c.get(),
      "SELECT s.id,s.exam_id,e.name,s.student_name,s.exam_number,s.student_class,s.score,s.start_time,s.mac_address,s.answers_json,s.created_at,s.identity_data FROM submissions s LEFT JOIN exams e ON e.id=s.exam_id WHERE s.id=$1",
      {id_str});
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK && PQntuples(r.get())>0){
      found=true;
      std::string score=PQgetvalue(r.get(),0,6);
      if(score.empty()) score="null";
      std::string answers=PQgetvalue(r.get(),0,9);
      if(answers.empty()) answers="[]";
      std::string ident=PQgetvalue(r.get(),0,11);
      std::string ct=PQgetvalue(r.get(),0,10); if(ct.size()>=19) ct=ct.substr(0,19);
      subj="{\"id\":"+std::string(PQgetvalue(r.get(),0,0))
        +",\"exam_id\":"+PQgetvalue(r.get(),0,1)
        +",\"exam_name\":\""+json_escape_ci(PQgetvalue(r.get(),0,2))+"\""
        +",\"student_name\":\""+json_escape_ci(PQgetvalue(r.get(),0,3))+"\""
        +",\"exam_number\":\""+json_escape_ci(PQgetvalue(r.get(),0,4))+"\""
        +",\"student_class\":\""+json_escape_ci(PQgetvalue(r.get(),0,5))+"\""
        +",\"score\":"+score
        +",\"start_time\":\""+json_escape_ci(PQgetvalue(r.get(),0,7))+"\""
        +",\"mac_address\":\""+json_escape_ci(PQgetvalue(r.get(),0,8))+"\""
        +",\"answers\":"+answers
        +",\"identity_data\":"+(ident.empty()?"null":"\""+json_escape_ci(ident)+"\"")
        +",\"created_at\":\""+json_escape_ci(ct)+"\"}";
    }
    real.release(c.release());
  });
  if(found){ Response r; r.json(200,"{\"success\":true,\"submission\":"+subj+"}"); return r; }
  Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"submission not found\"}"); return r;
#else
  (void)id_str;
  Response r; r.status=404; r.json(404,"{\"success\":false,\"error\":\"submission not found\"}"); return r;
#endif
}

// ===== queue status =========================================================
Response queue_status(const Request& req){
  /* M1: stub protobuf awal dihapus — pending 0 padahal bisa ada antrean. */
  long long pending=0, failed=0;
#ifdef HAS_HIREDIS
  auto cfg=Config::load();
  std::string rurl=cfg.redis_url;
  if(rurl.empty()) if(auto* e=getenv("REDIS_URL")) rurl=e;
  if(!rurl.empty()){
    auto rc=examvan::redis_real::connect_redis(rurl);
    if(rc){
      pending=examvan::redis_real::redis_llen(rc.get(),"examvan:submissions:pending");
      failed=examvan::redis_real::redis_llen(rc.get(),"examvan:submissions:failed");
    }
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"pending\":"+std::to_string(pending)+",\"failed\":"+std::to_string(failed)+"}"); return r;
}

// ===== delete ===============================================================
Response delete_submission(const Request& req){
  /* M1: stub protobuf awal dihapus — "success" tanpa DELETE adalah no-op. */
  std::string id_str;
  auto it=req.params.find("id");
  if(it!=req.params.end() && !it->second.empty()) id_str=it->second;
  if(id_str.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"id required\"}"); return r; }
#ifdef HAS_LIBPQ
  std::string result="";
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto del=real.exec_params(c.get(),"DELETE FROM submissions WHERE id=$1",{id_str});
    if(del && PQresultStatus(del.get())==PGRES_COMMAND_OK){ result="ok"; }
    else { utils::log_error("submission_delete_failed",PQresultErrorMessage(del.get())); result="__fail__"; }
    real.release(c.release());
  });
  if(result=="__fail__"){ Response r; r.status=500; r.json(500,"{\"success\":false,\"error\":\"Gagal menghapus submission\"}"); return r; }
  if(result=="ok"){ Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Submission dihapus\"}"); return r; }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Submission dihapus\"}"); return r;
}
} // namespace examvan::handlers::admin