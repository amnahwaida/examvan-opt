#include "handlers/admin/pengawas.hpp"
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
#include <string>
#include <cctype>
#include <optional>
#include <fstream>
#include <sstream>

namespace examvan::handlers::admin {

static std::string get_param(const std::map<std::string,std::string>& form, const std::string& key){
  auto it=form.find(key); return it!=form.end()? it->second : "";
}

static int session_admin_id_from(const Request& req){
  for(auto& kv:req.headers){ std::string k=kv.first; for(char& ch:k) ch=tolower((unsigned char)ch); if(k=="x-internal-admin-id"){ try{ return std::stoi(kv.second); }catch(...){} } }
  return 0;
}

#ifdef HAS_LIBPQ
static std::string json_escape_pw(const std::string& s){
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

Response pengawas_page(const Request&){
  std::string html=render_admin_template("pengawas","2.7.2");
  if(!html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Pengawas</h1></body></html>"; return r;
}

Response pengawas_detail_page(const Request&){
  std::string html=render_admin_template("pengawas_detail","2.7.2");
  if(!html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Pengawas Detail</h1></body></html>"; return r;
}

// ===== daftar ujian (pengawas) ==============================================
Response pengawas_exams(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::PengawasExamList pb; pb.set_success(true); pb.set_is_privileged(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  auto q=helpers::parse_form(req.query);
  int page=1, per_page=10;
  try{ page=std::stoi(get_param(q,"page")); }catch(...){}
  try{ per_page=std::stoi(get_param(q,"per_page")); }catch(...){}
  if(page<1) page=1;
  if(per_page<5) per_page=5; else if(per_page>50) per_page=50;
  std::string search=get_param(q,"search");
  int uid=session_admin_id_from(req);
  (void)uid;
  std::string pagination="{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)+",\"total\":0,\"total_pages\":0}";
  std::string stats="{\"total_exams\":0,\"active_exams\":0,\"total_students\":0,\"total_submitted\":0}";
  bool is_priv=false;
  (void)is_priv;
#ifdef HAS_LIBPQ
  {
    auto cfg2=Config::load();
    std::string db2=cfg2.database_url;
    if(db2.empty()) if(auto* e=getenv("DATABASE_URL")) db2=e;
    if(!db2.empty()){
      examvan::DbPool p2(db2, 2);
      examvan::db::RealPool rp2(examvan::conninfo_from_url_or_raw(p2.url), 2);
      if(auto c2=rp2.acquire()){
        if(PQstatus(c2.get())==CONNECTION_OK){
          auto me=rp2.exec_params(c2.get(),"SELECT role FROM admin_users WHERE id=$1",{std::to_string(uid)});
          if(me && PQresultStatus(me.get())==PGRES_TUPLES_OK && PQntuples(me.get())>0){
            std::string role=PQgetvalue(me.get(),0,0);
            if(role.find("superadmin")!=std::string::npos || role.find("operator")!=std::string::npos) is_priv=true;
          }
        }
        rp2.release(c2.release());
      }
    }
  }
  std::string arr="[]";
  bool got=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    // ujian yang diampu: exam_pengawas JOIN exams (untuk pengawas biasa).
    // Superadmin/operator melihat semua.
    std::string where=" WHERE 1=1";
    std::vector<std::string> params;

    if(!is_priv){
      where+=" AND e.id IN (SELECT exam_id FROM exam_pengawas WHERE user_id=$"+std::to_string(params.size()+1)+")";
      params.push_back(std::to_string(uid));
    }
    if(!search.empty()){
      where+=" AND e.name ILIKE $"+std::to_string(params.size()+1);
      params.push_back("%"+search+"%");
    }
    std::string sql="SELECT e.id,e.name,e.token,e.active_token,e.status,e.start_time,e.end_time,e.created_by,e.created_at,e.auto_approve,"
      "COALESCE((SELECT COUNT(*) FROM student_access_logs l WHERE l.exam_id=e.id),0) AS total_students,"
      "COALESCE((SELECT COUNT(*) FROM submissions s WHERE s.exam_id=e.id),0) AS submitted_count,"
      "COUNT(*) OVER() AS total"
      " FROM exams e"+where+" ORDER BY e.id DESC"
      " LIMIT $"+std::to_string(params.size()+1)+" OFFSET $"+std::to_string(params.size()+2);
    params.push_back(std::to_string(per_page));
    params.push_back(std::to_string((page-1)*per_page));
    auto r=real.exec_params(c.get(),sql,params);
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      int n=PQntuples(r.get());
      int total=0; if(n>0) total=std::atoi(PQgetvalue(r.get(),0,12));
      int total_pages=(total+per_page-1)/per_page;
      std::string s="[";
      for(int i=0;i<n;i++){
        if(i>0) s+=",";
        std::string st=PQgetvalue(r.get(),i,4);
        std::string created=PQgetvalue(r.get(),i,8); if(created.size()>=19) created=created.substr(0,19);
        s+="{\"id\":"+std::string(PQgetvalue(r.get(),i,0))
          +",\"name\":\""+json_escape_pw(PQgetvalue(r.get(),i,1))+"\""
          +",\"token\":\""+json_escape_pw(PQgetvalue(r.get(),i,2))+"\""
          +",\"active_token\":\""+json_escape_pw(PQgetvalue(r.get(),i,3))+"\""
          +",\"status\":\""+json_escape_pw(st)+"\""
          +",\"start_time\":\""+json_escape_pw(PQgetvalue(r.get(),i,5))+"\""
          +",\"end_time\":\""+json_escape_pw(PQgetvalue(r.get(),i,6))+"\""
          +",\"created_by\":"+PQgetvalue(r.get(),i,7)
          +",\"created_at\":\""+json_escape_pw(created)+"\""
          +",\"auto_approve\":"+(std::string(PQgetvalue(r.get(),i,9))=="t"?"true":"false")
          +",\"total_students\":"+PQgetvalue(r.get(),i,10)
          +",\"submitted_count\":"+PQgetvalue(r.get(),i,11)+"}";
      }
      s+="]"; arr=s;
      pagination="{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)
        +",\"total\":"+std::to_string(total)+",\"total_pages\":"+std::to_string(total_pages)+"}";
      got=true;
    }
    // stats global (untuk header)
    auto st=real.exec_params(c.get(),
      "SELECT COUNT(*) FROM exams e WHERE 1=1"
      +std::string(!is_priv? " AND e.id IN (SELECT exam_id FROM exam_pengawas WHERE user_id="+std::to_string(uid)+")" : ""),{});
    if(st && PQresultStatus(st.get())==PGRES_TUPLES_OK && PQntuples(st.get())>0){
      int total_exams=std::atoi(PQgetvalue(st.get(),0,0));
      auto st2=real.exec_params(c.get(),
        "SELECT COUNT(*) FROM exams e WHERE e.status='active'"
        +std::string(!is_priv? " AND e.id IN (SELECT exam_id FROM exam_pengawas WHERE user_id="+std::to_string(uid)+")" : ""),{});
      int active= st2 && PQresultStatus(st2.get())==PGRES_TUPLES_OK && PQntuples(st2.get())>0? std::atoi(PQgetvalue(st2.get(),0,0)) : 0;
      auto st3=real.exec_params(c.get(),
        "SELECT COUNT(*) FROM student_access_logs l LEFT JOIN exams e ON e.id=l.exam_id WHERE 1=1"
        +std::string(!is_priv? " AND e.id IN (SELECT exam_id FROM exam_pengawas WHERE user_id="+std::to_string(uid)+")" : ""),{});
      int students= st3 && PQresultStatus(st3.get())==PGRES_TUPLES_OK && PQntuples(st3.get())>0? std::atoi(PQgetvalue(st3.get(),0,0)) : 0;
      auto st4=real.exec_params(c.get(),
        "SELECT COUNT(*) FROM submissions s LEFT JOIN exams e ON e.id=s.exam_id WHERE 1=1"
        +std::string(!is_priv? " AND e.id IN (SELECT exam_id FROM exam_pengawas WHERE user_id="+std::to_string(uid)+")" : ""),{});
      int submitted= st4 && PQresultStatus(st4.get())==PGRES_TUPLES_OK && PQntuples(st4.get())>0? std::atoi(PQgetvalue(st4.get(),0,0)) : 0;
      stats="{\"total_exams\":"+std::to_string(total_exams)+",\"active_exams\":"+std::to_string(active)
        +",\"total_students\":"+std::to_string(students)+",\"total_submitted\":"+std::to_string(submitted)+"}";
    }
    real.release(c.release());
  });
  if(got){
    Response r; r.json(200,"{\"success\":true,\"exams\":"+arr+",\"is_privileged\":"+(is_priv?"true":"false")
      +",\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)
      +",\"stats\":"+stats+"}");
    return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"exams\":[],\"is_privileged\":true,\"page\":1,\"per_page\":10,\"total\":0,\"total_pages\":0,\"stats\":"+stats+"}"); return r;
}

// ===== submissions per exam =================================================
Response pengawas_submissions(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::PengawasSubmissionList pb; pb.set_success(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  std::string exam_id;
  auto it=req.params.find("exam_id");
  if(it!=req.params.end() && !it->second.empty()) exam_id=it->second;
  if(exam_id.empty()){
    auto q=helpers::parse_form(req.query);
    exam_id=get_param(q,"exam_id");
  }
  if(exam_id.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"exam id required\"}"); return r; }
  auto q=helpers::parse_form(req.query);
  int page=1, per_page=20;
  try{ page=std::stoi(get_param(q,"page")); }catch(...){}
  try{ per_page=std::stoi(get_param(q,"per_page")); }catch(...){}
  if(page<1) page=1;
  if(per_page<1) per_page=20; else if(per_page>200) per_page=200;
#ifdef HAS_LIBPQ
  std::string arr="[]", exam_name="", active_token="";
  (void)exam_name; (void)active_token;
  int total_all=0, total_pages_all=0;
  bool got=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto ex=real.exec_params(c.get(),"SELECT name,active_token FROM exams WHERE id=$1",{exam_id});
    if(ex && PQresultStatus(ex.get())==PGRES_TUPLES_OK && PQntuples(ex.get())>0){
      exam_name=PQgetvalue(ex.get(),0,0);
      active_token=PQgetvalue(ex.get(),0,1);
    }
    std::string sql="SELECT s.id,s.student_name,s.exam_number,s.student_class,s.score,s.start_time,s.mac_address,s.created_at, COUNT(*) OVER() AS total"
      " FROM submissions s WHERE s.exam_id=$1 ORDER BY s.id DESC"
      " LIMIT $2 OFFSET $3";
    auto r=real.exec_params(c.get(),sql,{exam_id,std::to_string(per_page),std::to_string((page-1)*per_page)});
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      int n=PQntuples(r.get());
      int total=0; if(n>0) total=std::atoi(PQgetvalue(r.get(),0,8));
      total_all=total; total_pages_all=(total+per_page-1)/per_page;
      std::string s="[";
      for(int i=0;i<n;i++){
        if(i>0) s+=",";
        std::string ct=PQgetvalue(r.get(),i,7); if(ct.size()>=19) ct=ct.substr(0,19);
        std::string score=PQgetvalue(r.get(),i,4); if(score.empty()) score="null";
        s+="{\"id\":"+std::string(PQgetvalue(r.get(),i,0))
          +",\"student_name\":\""+json_escape_pw(PQgetvalue(r.get(),i,1))+"\""
          +",\"exam_number\":\""+json_escape_pw(PQgetvalue(r.get(),i,2))+"\""
          +",\"student_class\":\""+json_escape_pw(PQgetvalue(r.get(),i,3))+"\""
          +",\"score\":"+score
          +",\"start_time\":\""+json_escape_pw(PQgetvalue(r.get(),i,5))+"\""
          +",\"mac_address\":\""+json_escape_pw(PQgetvalue(r.get(),i,6))+"\""
          +",\"created_at\":\""+json_escape_pw(ct)+"\"}";
      }
      s+="]"; arr=s;
      got=true;
    }
    real.release(c.release());
  });
  if(got){
    Response r; r.json(200,"{\"success\":true,\"exam_name\":\""+json_escape_pw(exam_name)+"\",\"exam_active_token\":\""+json_escape_pw(active_token)
      +"\",\"submissions\":"+arr+",\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)
      +",\"total\":"+std::to_string(total_all)+",\"total_pages\":"+std::to_string(total_pages_all)+",\"stats\":{}}");
    return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"exam_name\":\"\",\"exam_active_token\":\"\",\"submissions\":[],\"page\":1,\"per_page\":20,\"total\":0,\"total_pages\":0,\"stats\":{}}"); return r;
}

// ===== approvals ============================================================
Response pending_approvals(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::ApprovalList pb; pb.set_success(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  std::string exam_id;
  auto it=req.params.find("exam_id");
  if(it!=req.params.end() && !it->second.empty()) exam_id=it->second;
  if(exam_id.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"exam id required\"}"); return r; }
  auto q=helpers::parse_form(req.query);
  int page=1, limit=100;
  try{ page=std::stoi(get_param(q,"page")); }catch(...){}
  try{ limit=std::stoi(get_param(q,"limit")); }catch(...){}
  if(page<1) page=1;
  if(limit<1) limit=100; else if(limit>500) limit=500;
#ifdef HAS_LIBPQ
  std::string arr="[]";
  int total_all=0;
  bool got=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto r=real.exec_params(c.get(),
      "SELECT id,mac_address,student_name,exam_number,student_class,identity_data,status,created_at, COUNT(*) OVER() AS total FROM exam_approvals WHERE exam_id=$1 AND status='pending' ORDER BY created_at DESC LIMIT $2 OFFSET $3",
      {exam_id,std::to_string(limit),std::to_string((page-1)*limit)});
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
      got=true;
      int n=PQntuples(r.get());
      int total=0; if(n>0) total=std::atoi(PQgetvalue(r.get(),0,8));
      total_all=total;
      std::string s="[";
      for(int i=0;i<n;i++){
        if(i>0) s+=",";
        std::string ct=PQgetvalue(r.get(),i,7); if(ct.size()>=19) ct=ct.substr(0,19);
        s+="{\"id\":"+std::string(PQgetvalue(r.get(),i,0))
          +",\"mac_address\":\""+json_escape_pw(PQgetvalue(r.get(),i,1))+"\""
          +",\"student_name\":\""+json_escape_pw(PQgetvalue(r.get(),i,2))+"\""
          +",\"exam_number\":\""+json_escape_pw(PQgetvalue(r.get(),i,3))+"\""
          +",\"student_class\":\""+json_escape_pw(PQgetvalue(r.get(),i,4))+"\""
          +",\"identity_data\":\""+json_escape_pw(PQgetvalue(r.get(),i,5))+"\""
          +",\"status\":\""+json_escape_pw(PQgetvalue(r.get(),i,6))+"\""
          +",\"created_at\":\""+json_escape_pw(ct)+"\"}";
      }
      s+="]"; arr=s;
    }
    real.release(c.release());
  });
  if(got){
    Response r; r.json(200,"{\"success\":true,\"data\":"+arr+",\"total\":"+std::to_string(total_all)+",\"page\":"+std::to_string(page)+",\"limit\":"+std::to_string(limit)+"}");
    return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"data\":[],\"total\":0,\"page\":1,\"limit\":100}"); return r;
}

Response set_approval(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::SetApprovalResponse pb; pb.set_success(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  std::string exam_id, mac;
  auto it=req.params.find("exam_id");
  if(it!=req.params.end() && !it->second.empty()) exam_id=it->second;
  auto it2=req.params.find("mac_address");
  if(it2!=req.params.end() && !it2->second.empty()) mac=it2->second;
  if(exam_id.empty()||mac.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"exam id dan mac_address wajib\"}"); return r; }
  auto form=helpers::parse_form(req.body);
  std::string status=get_param(form,"status");
  if(status.empty()) status="approved";
  if(status!="approved" && status!="rejected" && status!="pending"){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"status tidak valid\"}"); return r; }
#ifdef HAS_LIBPQ
  bool done=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto up=real.exec_params(c.get(),
      "UPDATE exam_approvals SET status=$3,updated_at=now() WHERE exam_id=$1 AND mac_address=$2",
      {exam_id,mac,status});
    if(up && (PQresultStatus(up.get())==PGRES_COMMAND_OK||PQresultStatus(up.get())==PGRES_TUPLES_OK)) done=true;
    real.release(c.release());
  });
  if(done){ Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Persetujuan diperbarui\"}"); return r; }
#endif
  Response r; r.json(200,"{\"success\":true,\"ok\":true,\"message\":\"Persetujuan diperbarui\"}"); return r;
}

Response get_auto_approve(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::AutoApproveResponse pb; pb.set_success(true); pb.set_enabled(false);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  std::string exam_id;
  auto it=req.params.find("exam_id");
  if(it!=req.params.end() && !it->second.empty()) exam_id=it->second;
  if(exam_id.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"exam id required\"}"); return r; }
  std::string enabled="false";
#ifdef HAS_LIBPQ
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto r=real.exec_params(c.get(),"SELECT auto_approve FROM exams WHERE id=$1",{exam_id});
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK && PQntuples(r.get())>0){
      enabled=std::string(PQgetvalue(r.get(),0,0))=="t"?"true":"false";
    }
    real.release(c.release());
  });
#endif
  Response r; r.json(200,"{\"success\":true,\"enabled\":"+enabled+"}"); return r;
}

Response set_auto_approve(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::SetAutoApproveResponse pb; pb.set_success(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  std::string exam_id;
  auto it=req.params.find("exam_id");
  if(it!=req.params.end() && !it->second.empty()) exam_id=it->second;
  if(exam_id.empty()){ Response r; r.status=400; r.json(400,"{\"success\":false,\"error\":\"exam id required\"}"); return r; }
  auto form=helpers::parse_form(req.body);
  std::string en=get_param(form,"enabled");
  if(en.empty()){ auto q=helpers::parse_form(req.query); en=get_param(q,"enabled"); }
  std::string val = (en=="1"||en=="true") ? "true" : "false";
#ifdef HAS_LIBPQ
  bool done=false;
  with_pg([&](examvan::db::RealPool& real){
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
    auto up=real.exec_params(c.get(),"UPDATE exams SET auto_approve=$2 WHERE id=$1",{exam_id,val});
    if(up && (PQresultStatus(up.get())==PGRES_COMMAND_OK||PQresultStatus(up.get())==PGRES_TUPLES_OK)) done=true;
    real.release(c.release());
  });
  if(done){ Response r; r.json(200,"{\"success\":true,\"enabled\":"+val+",\"message\":\"Auto-approve diperbarui\"}"); return r; }
#endif
  Response r; r.json(200,"{\"success\":true,\"enabled\":"+val+"}"); return r;
}
} // namespace examvan::handlers::admin