#include "store/exam_store_postgres.hpp"
#ifdef HAS_LIBPQ
#include <libpq-fe.h>
#include <algorithm>
#include <cstdlib>
#include "utils/log.hpp"

namespace examvan::store {
namespace {
constexpr const char* kColumns = "id,name,file_path,size_bytes,token,active_token,questions_json,status,security_level,strict_mode,public_results,show_answers,created_by,created_at,identity_fields,panel_color,start_time,end_time,delegated_to,token_mode,token_reset_interval,token_last_reset_at,exam_started_at,tombstoned_at,congrats_message,auto_approve";
constexpr const char* kSeq = "CREATE SEQUENCE IF NOT EXISTS exams_id_seq";
constexpr const char* kTableExams = R"SQL(CREATE TABLE IF NOT EXISTS exams (
 id INTEGER PRIMARY KEY DEFAULT nextval('exams_id_seq'),
 name TEXT NOT NULL,
 file_path TEXT NOT NULL,
 size_bytes BIGINT NOT NULL DEFAULT 0,
 token TEXT NOT NULL UNIQUE,
 active_token TEXT NOT NULL,
 questions_json TEXT,
 status TEXT NOT NULL DEFAULT 'inactive',
 security_level TEXT NOT NULL DEFAULT 'medium',
 strict_mode INTEGER NOT NULL DEFAULT 0,
 public_results INTEGER NOT NULL DEFAULT 1,
 show_answers INTEGER NOT NULL DEFAULT 1,
 created_by INTEGER NOT NULL DEFAULT 0,
 created_at TEXT NOT NULL,
 identity_fields TEXT,
 panel_color TEXT,
 start_time TEXT,
 end_time TEXT,
 delegated_to INTEGER,
 token_mode TEXT,
 token_reset_interval INTEGER,
 token_last_reset_at TEXT,
 exam_started_at TEXT,
 tombstoned_at TEXT,
 congrats_message TEXT,
 auto_approve BOOLEAN NOT NULL DEFAULT FALSE
))SQL";
constexpr const char* kIdx1 = "CREATE INDEX IF NOT EXISTS exams_active_token_idx ON exams(active_token)";
constexpr const char* kIdx2 = "CREATE INDEX IF NOT EXISTS exams_created_at_idx ON exams(created_at DESC)";
constexpr const char* kTableIdem = R"SQL(CREATE TABLE IF NOT EXISTS exam_idempotency (
 idempotency_key TEXT PRIMARY KEY,
 request_fingerprint TEXT NOT NULL,
 exam_id INTEGER NOT NULL REFERENCES exams(id) ON DELETE CASCADE,
 created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
))SQL";
}

bool ExamStorePostgres::exec_command(const std::string& sql, const std::vector<std::string>& params){
  auto result=pool_.exec_params_pooled(sql,params);
  return result && (PQresultStatus(result.get())==PGRES_COMMAND_OK || PQresultStatus(result.get())==PGRES_TUPLES_OK);
}

bool ExamStorePostgres::exec_command_nullable(const std::string& sql,const std::vector<std::optional<std::string>>& params){
  auto c=pool_.acquire();
  if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
  auto result=pool_.exec_params_nullable(c.get(),sql,params);
  const bool ok=result && (PQresultStatus(result.get())==PGRES_COMMAND_OK || PQresultStatus(result.get())==PGRES_TUPLES_OK);
  if(!ok){
    // Log error asli PG (mis. FK violation) — sebelumnya gagal senyap dan
    // create_exam melaporkan "custom_token already in use" yang menyesatkan.
    std::string err = result ? PQresultErrorMessage(result.get()) : "no result";
    utils::log_error("pg_exec_failed", "sql="+sql.substr(0,120)+" err="+err);
  }
  pool_.release(c.release());
  return ok;
}

std::optional<std::string> ExamStorePostgres::nullable(PGresult* result,int row,int col){
  if(PQgetisnull(result,row,col)) return std::nullopt;
  return std::string(PQgetvalue(result,row,col));
}

models::Exam ExamStorePostgres::map_exam(PGresult* r,int row){
  models::Exam e;
  auto value=[&](int col){ return std::string(PQgetvalue(r,row,col)); };
  e.id=std::stoi(value(0)); e.name=value(1); e.file_path=value(2); e.size_bytes=std::stoll(value(3));
  e.token=value(4); e.active_token=value(5); e.questions_json=nullable(r,row,6); e.status=value(7); e.security_level=value(8);
  e.strict_mode=std::stoi(value(9)); e.public_results=std::stoi(value(10)); e.show_answers=std::stoi(value(11)); e.created_by=std::stoi(value(12)); e.created_at=value(13);
  e.identity_fields=nullable(r,row,14); e.panel_color=nullable(r,row,15); e.start_time=nullable(r,row,16); e.end_time=nullable(r,row,17);
  if(auto v=nullable(r,row,18)) e.delegated_to=std::stoi(*v);
  e.token_mode=nullable(r,row,19);
  if(auto v=nullable(r,row,20)) e.token_reset_interval=std::stoi(*v);
  e.token_last_reset_at=nullable(r,row,21); e.exam_started_at=nullable(r,row,22); e.tombstoned_at=nullable(r,row,23); e.congrats_message=nullable(r,row,24);
  e.auto_approve=value(25)=="t" || value(25)=="true" || value(25)=="1";
  return e;
}

std::vector<models::Exam> ExamStorePostgres::query_exams(const std::string& sql,const std::vector<std::string>& params){
  std::vector<models::Exam> out;
  auto c=pool_.acquire();
  if(!c || PQstatus(c.get())!=CONNECTION_OK) return out;
  auto result=pool_.exec_params(c.get(),sql,params);
  pool_.release(c.release());
  if(!result || PQresultStatus(result.get())!=PGRES_TUPLES_OK) return out;
  for(int row=0;row<PQntuples(result.get());++row) out.push_back(map_exam(result.get(),row));
  return out;
}

bool ExamStorePostgres::execute_transaction(const std::vector<std::pair<std::string,std::vector<std::string>>>& statements){
  auto c=pool_.acquire();
  if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
  auto begin=pool_.exec_params(c.get(),"BEGIN",{});
  if(!begin || PQresultStatus(begin.get())!=PGRES_COMMAND_OK){ pool_.release(c.release()); return false; }
  for(const auto& statement: statements){
    auto result=pool_.exec_params(c.get(),statement.first,statement.second);
    if(!result || PQresultStatus(result.get())!=PGRES_COMMAND_OK){
      (void)pool_.exec_params(c.get(),"ROLLBACK",{});
      pool_.release(c.release());
      return false;
    }
  }
  auto commit=pool_.exec_params(c.get(),"COMMIT",{});
  const bool ok=commit && PQresultStatus(commit.get())==PGRES_COMMAND_OK;
  pool_.release(c.release());
  return ok;
}

std::optional<std::string> ExamStorePostgres::execute_transaction_result(
    const std::vector<std::pair<std::string,std::vector<std::string>>>& statements){
  auto c=pool_.acquire();
  if(!c || PQstatus(c.get())!=CONNECTION_OK) return std::nullopt;
  auto begin=pool_.exec_params(c.get(),"BEGIN",{});
  if(!begin || PQresultStatus(begin.get())!=PGRES_COMMAND_OK){ pool_.release(c.release()); return std::nullopt; }
  PGresult* last_result=nullptr;
  for(const auto& stmt: statements){
    auto r=pool_.exec_params(c.get(),stmt.first,stmt.second);
    if(!r || (PQresultStatus(r.get())!=PGRES_COMMAND_OK && PQresultStatus(r.get())!=PGRES_TUPLES_OK)){
      (void)pool_.exec_params(c.get(),"ROLLBACK",{});
      pool_.release(c.release());
      return std::nullopt;
    }
    last_result=r.get();
  }
  // Extract first column of first row from last result (INSERT ... RETURNING id)
  std::optional<std::string> out;
  if(last_result && PQntuples(last_result)>0 && !PQgetisnull(last_result,0,0))
    out=std::string(PQgetvalue(last_result,0,0));
  auto commit=pool_.exec_params(c.get(),"COMMIT",{});
  if(!commit || PQresultStatus(commit.get())!=PGRES_COMMAND_OK){
    pool_.release(c.release());
    return std::nullopt;
  }
  pool_.release(c.release());
  return out;
}

bool ExamStorePostgres::migrate(){
  std::lock_guard<std::mutex> lock(mu_);
  ready_=exec_command(kSeq) && exec_command(kTableExams) && exec_command(kIdx1) && exec_command(kIdx2) && exec_command(kTableIdem);
  // Backward-compatible migration: extend exam_idempotency for durable
  // idempotency (state machine, response replay, lease tracking).
  if(ready_){
    exec_command("ALTER TABLE exam_idempotency ALTER COLUMN exam_id DROP NOT NULL");
    exec_command("ALTER TABLE exam_idempotency DROP CONSTRAINT IF EXISTS exam_idempotency_exam_id_fkey");
    exec_command("ALTER TABLE exam_idempotency ADD COLUMN IF NOT EXISTS state TEXT NOT NULL DEFAULT 'completed'");
    exec_command("ALTER TABLE exam_idempotency ADD COLUMN IF NOT EXISTS response_status INTEGER");
    exec_command("ALTER TABLE exam_idempotency ADD COLUMN IF NOT EXISTS response_body TEXT");
    exec_command("ALTER TABLE exam_idempotency ADD COLUMN IF NOT EXISTS response_content_type TEXT");
    exec_command("ALTER TABLE exam_idempotency ADD COLUMN IF NOT EXISTS owner_id TEXT");
    exec_command("ALTER TABLE exam_idempotency ADD COLUMN IF NOT EXISTS updated_at TEXT");
    exec_command("ALTER TABLE exam_idempotency ADD COLUMN IF NOT EXISTS finalized_at TEXT");
    // Sync sequence PG setelah restore backup: restore hanya memulihkan data
    // (MAX(id)), bukan posisi sequence. Kalau sequence ketinggalan, nextval
    // mengembalikan id yang sudah dipakai → INSERT gagal (PK violation / 409).
    // setval(seq, MAX(id), is_called): is_called=false saat tabel kosong agar
    // nextval berikutnya = 1; is_called=true saat ada data agar nextval =
    // MAX(id)+1. Tabel yang belum ada (pg_get_serial_sequence → NULL) dilewati.
    for(const char* t: {"exams","submissions","student_access_logs",
                        "admin_users","vouchers","voucher_redemptions",
                        "exam_approvals"}){
      exec_command(
        "SELECT setval(pg_get_serial_sequence('"+std::string(t)+"','id'),"
        " COALESCE((SELECT MAX(id) FROM "+std::string(t)+"),1),"
        " EXISTS (SELECT 1 FROM "+std::string(t)+"))"
        " WHERE pg_get_serial_sequence('"+std::string(t)+"','id') IS NOT NULL");
    }
  }
  return ready_;
}

bool ExamStorePostgres::hydrate(){
  std::lock_guard<std::mutex> lock(mu_);
  if(!ready_) return false;
  auto c=pool_.acquire();
  if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
  auto result=pool_.exec_params(c.get(),std::string("SELECT ")+kColumns+" FROM exams LIMIT 1",{});
  const bool ok=result && PQresultStatus(result.get())==PGRES_TUPLES_OK;
  pool_.release(c.release());
  return ok;
}

bool ExamStorePostgres::ready() const { std::lock_guard<std::mutex> lock(mu_); return ready_; }

int ExamStorePostgres::next_id(){
  std::lock_guard<std::mutex> lock(mu_);
  auto result=pool_.exec_params_pooled("SELECT nextval(pg_get_serial_sequence('exams','id'))",{});
  if(!result || PQresultStatus(result.get())!=PGRES_TUPLES_OK || PQntuples(result.get())==0) return 1;
  try { return std::stoi(PQgetvalue(result.get(),0,0)); } catch(...) { return 1; }
}

bool ExamStorePostgres::add(const models::Exam& e){
  std::lock_guard<std::mutex> lock(mu_);
  const std::string sql="INSERT INTO exams ("+std::string(kColumns)+") VALUES ("+
    "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21,$22,$23,$24,$25,$26)";
  std::vector<std::optional<std::string>> p={std::to_string(e.id),e.name,e.file_path,std::to_string(e.size_bytes),e.token,e.active_token,
    e.questions_json,e.status,e.security_level,std::to_string(e.strict_mode),std::to_string(e.public_results),std::to_string(e.show_answers),std::to_string(e.created_by),e.created_at,
    e.identity_fields,e.panel_color,e.start_time,e.end_time,e.delegated_to?std::optional<std::string>(std::to_string(*e.delegated_to)):std::nullopt,
    e.token_mode,e.token_reset_interval?std::optional<std::string>(std::to_string(*e.token_reset_interval)):std::nullopt,e.token_last_reset_at,e.exam_started_at,e.tombstoned_at,e.congrats_message,e.auto_approve?"true":"false"};
  return exec_command_nullable(sql,p);
}

std::optional<models::Exam> ExamStorePostgres::get_by_id(int id){
  std::lock_guard<std::mutex> lock(mu_);
  auto rows=query_exams(std::string("SELECT ")+kColumns+" FROM exams WHERE id=$1",{std::to_string(id)});
  if(rows.empty()) return std::nullopt;
  return rows.front();
}
std::vector<models::Exam> ExamStorePostgres::list_all(){
  std::lock_guard<std::mutex> lock(mu_);
  return query_exams(std::string("SELECT ")+kColumns+" FROM exams ORDER BY id");
}
bool ExamStorePostgres::token_exists(const std::string& token,int exclude_id){
  std::lock_guard<std::mutex> lock(mu_);
  std::string sql="SELECT "+std::string(kColumns)+" FROM exams WHERE token=$1";
  std::vector<std::string> p={token}; if(exclude_id>0){sql+=" AND id<>$2";p.push_back(std::to_string(exclude_id));} sql+=" LIMIT 1";
  return !query_exams(sql,p).empty();
}
bool ExamStorePostgres::claim_token(const std::string& token){ return !token_exists(token); }
void ExamStorePostgres::unclaim_token(const std::string&) {}
bool ExamStorePostgres::update(int id,const std::function<void(models::Exam&)>& mutator){
  std::lock_guard<std::mutex> lock(mu_);
  auto rows=query_exams(std::string("SELECT ")+kColumns+" FROM exams WHERE id=$1",{std::to_string(id)}); if(rows.empty()) return false;
  mutator(rows.front()); const auto& e=rows.front();
  // Read-modify-write: seluruh kolom mutable ikut ditulis ulang supaya
  // update() tidak menjatuhkan field (questions_json, auto_approve, dll).
  const std::string sql="UPDATE exams SET name=$1,file_path=$2,size_bytes=$3,token=$4,active_token=$5,status=$6,token_mode=$7,token_reset_interval=$8,token_last_reset_at=$9,exam_started_at=$10,tombstoned_at=$11,questions_json=$12,security_level=$13,strict_mode=$14,public_results=$15,show_answers=$16,identity_fields=$17,panel_color=$18,start_time=$19,end_time=$20,congrats_message=$21,auto_approve=$22,delegated_to=$23 WHERE id=$24";
  return exec_command_nullable(sql,{e.name,e.file_path,std::to_string(e.size_bytes),e.token,e.active_token,e.status,e.token_mode,e.token_reset_interval?std::optional<std::string>(std::to_string(*e.token_reset_interval)):std::nullopt,e.token_last_reset_at,e.exam_started_at,e.tombstoned_at,
    e.questions_json,e.security_level,std::to_string(e.strict_mode),std::to_string(e.public_results),std::to_string(e.show_answers),
    e.identity_fields,e.panel_color,e.start_time,e.end_time,e.congrats_message,e.auto_approve?"true":"false",
    e.delegated_to?std::optional<std::string>(std::to_string(*e.delegated_to)):std::nullopt,std::to_string(id)});
}
bool ExamStorePostgres::remove(int id){ std::lock_guard<std::mutex> lock(mu_); return exec_command("DELETE FROM exams WHERE id=$1",{std::to_string(id)}); }
size_t ExamStorePostgres::count(){ std::lock_guard<std::mutex> lock(mu_); auto rows=query_exams("SELECT "+std::string(kColumns)+" FROM exams"); return rows.size(); }
void ExamStorePostgres::clear_all(){ std::lock_guard<std::mutex> lock(mu_); (void)exec_command("DELETE FROM exams"); (void)exec_command("DELETE FROM exam_idempotency"); }

IdempotencyResult ExamStorePostgres::reserve_idempotency(const std::string& key, const std::string& fingerprint){
  std::lock_guard<std::mutex> lock(mu_);
  // Attempt atomic insert: ON CONFLICT DO NOTHING.
  // exam_id=0 is a placeholder (removed after finalization).
  const std::string insert_sql=
    "INSERT INTO exam_idempotency (idempotency_key,request_fingerprint,exam_id,state,created_at,updated_at)"
    " VALUES ($1,$2,0,'pending',NOW(),NOW()) ON CONFLICT (idempotency_key) DO NOTHING";
  auto c=pool_.acquire();
  if(!c || PQstatus(c.get())!=CONNECTION_OK) return {IdempotencyStatus::Conflict, "", 0, ""};
  auto ir=pool_.exec_params(c.get(),insert_sql,{key,fingerprint});
  if(!ir){
    pool_.release(c.release());
    return {IdempotencyStatus::Conflict, "", 0, ""};
  }
  // If INSERT succeeded (not just ON CONFLICT DO NOTHING) → NEW.
  if(PQresultStatus(ir.get())==PGRES_COMMAND_OK && std::string(PQcmdTuples(ir.get()))=="1"){
    pool_.release(c.release());
    return {IdempotencyStatus::New, "", 0, ""};
  }
  // ON CONFLICT DO NOTHING fired → key exists.  Look up the existing record.
  const std::string sel_sql=
    "SELECT request_fingerprint,state,response_body,response_status,response_content_type"
    " FROM exam_idempotency WHERE idempotency_key=$1";
  auto sr=pool_.exec_params(c.get(),sel_sql,{key});
  pool_.release(c.release());
  if(!sr || PQresultStatus(sr.get())!=PGRES_TUPLES_OK || PQntuples(sr.get())==0)
    return {IdempotencyStatus::Conflict, "", 0, ""};
  std::string existing_fp=PQgetvalue(sr.get(),0,0);
  std::string state=PQgetvalue(sr.get(),0,1);
  if(state=="completed" && existing_fp==fingerprint){
    IdempotencyResult res;
    res.status=IdempotencyStatus::Replay;
    res.response_body=PQgetvalue(sr.get(),0,2);
    res.response_status=std::stoi(PQgetvalue(sr.get(),0,3));
    res.response_content_type=PQgetvalue(sr.get(),0,4);
    return res;
  }
  return {IdempotencyStatus::Conflict, "", 0, ""};
}

void ExamStorePostgres::finalize_idempotency(const std::string& key, const std::string& fingerprint,
                                              int http_status, const std::string& response_body,
                                              const std::string& content_type){
  std::lock_guard<std::mutex> lock(mu_);
  int exam_id=0;
  {
    std::string needle="\"id\":";
    auto p=response_body.find(needle);
    if(p!=std::string::npos){
      size_t s=p+needle.size();
      while(s<response_body.size() && (response_body[s]==' '||response_body[s]=='\t')) ++s;
      size_t e=s;
      while(e<response_body.size() && isdigit((unsigned char)response_body[e])) ++e;
      if(e>s) try{ exam_id=std::stoi(response_body.substr(s,e-s)); }catch(...){}
    }
  }
  const std::string upd_sql=
    "UPDATE exam_idempotency SET exam_id=$1,response_status=$2,response_body=$3,"
    "response_content_type=$4,state='completed',finalized_at=NOW(),updated_at=NOW()"
    " WHERE idempotency_key=$5 AND request_fingerprint=$6";
  auto c=pool_.acquire();
  if(!c || PQstatus(c.get())!=CONNECTION_OK) return;
  auto ur=pool_.exec_params(c.get(),upd_sql,{std::to_string(exam_id),std::to_string(http_status),response_body,content_type,key,fingerprint});
  if(!ur || PQresultStatus(ur.get())!=PGRES_COMMAND_OK){
    pool_.release(c.release());
    return;
  }
  if(std::string(PQcmdTuples(ur.get()))=="0"){
    (void)pool_.exec_params(c.get(),
      "INSERT INTO exam_idempotency (idempotency_key,request_fingerprint,exam_id,state,response_status,response_body,response_content_type,created_at,updated_at,finalized_at)"
      " VALUES ($1,$2,$3,'completed',$4,$5,$6,NOW(),NOW(),NOW()) ON CONFLICT (idempotency_key) DO UPDATE SET"
      " exam_id=EXCLUDED.exam_id,response_status=EXCLUDED.response_status,response_body=EXCLUDED.response_body,"
      "response_content_type=EXCLUDED.response_content_type,state='completed',updated_at=NOW(),finalized_at=NOW()",
      {key,fingerprint,std::to_string(exam_id),std::to_string(http_status),response_body,content_type});
  }
  pool_.release(c.release());
}

void ExamStorePostgres::release_idempotency(const std::string& key){
  std::lock_guard<std::mutex> lock(mu_);
  (void)exec_command("DELETE FROM exam_idempotency WHERE idempotency_key=$1",{key});
}
}
#endif
