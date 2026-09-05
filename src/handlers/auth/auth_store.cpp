#include "handlers/auth/auth_store.hpp"
#include "config/config.hpp"
#include <cctype>
#ifdef HAS_LIBPQ
#include "db/pool.hpp"
#include "db/pool_real.hpp"
#include <libpq-fe.h>
#endif

namespace examvan::handlers::auth {

// ===================== mode unit test (EXAMVAN_TESTING) =====================
#ifdef EXAMVAN_TESTING
namespace {
std::unordered_map<std::string, RegisteredUser> g_users;
std::mutex g_mu;
int g_next_id = 100;
}

static bool ci_eq(const std::string& a, const std::string& b){
  if(a.size()!=b.size()) return false;
  for(size_t i=0;i<a.size();++i)
    if(tolower((unsigned char)a[i])!=tolower((unsigned char)b[i])) return false;
  return true;
}

void set_registered_user_for_test(const std::string& username, const std::string& email,
                                  const std::string& password_hash, const std::string& status,
                                  const std::string& otp_code, int otp_attempts,
                                  long otp_expiry_epoch){
  RegisteredUser u;
  u.id = g_next_id++;
  u.username = username;
  u.email = email;
  u.password_hash = password_hash;
  u.status = status;
  u.otp_code = otp_code;
  u.otp_attempts = otp_attempts;
  u.otp_expiry_epoch = otp_expiry_epoch;
  std::lock_guard<std::mutex> g(g_mu);
  g_users[username] = u;
}
void clear_registered_users_for_test(){ std::lock_guard<std::mutex> g(g_mu); g_users.clear(); }

bool find_registered_user(const std::string& username, RegisteredUser& out){
  std::lock_guard<std::mutex> g(g_mu);
  for(const auto& kv: g_users){
    if(ci_eq(kv.first, username)){ out=kv.second; return true; }
  }
  return false;
}
bool find_registered_user_by_email(const std::string& email, RegisteredUser& out){
  std::lock_guard<std::mutex> g(g_mu);
  for(const auto& kv: g_users){
    if(!kv.second.email.empty() && ci_eq(kv.second.email, email)){ out=kv.second; return true; }
  }
  return false;
}
int count_recent_registrations_by_ip(const std::string& ip){ (void)ip; return 0; }

bool insert_registered_user(const RegisteredUser& u){
  if(u.username.empty() || u.password_hash.empty()) return false;
  {
    RegisteredUser existing;
    if(find_registered_user(u.username, existing)) return false;
    if(find_registered_user_by_email(u.email, existing)) return false;
  }
  std::lock_guard<std::mutex> g(g_mu);
  if(g_users.count(u.username)) return false;
  RegisteredUser copy=u;
  if(copy.id==0) copy.id=g_next_id++;
  g_users[u.username]=copy;
  return true;
}
bool update_user_otp(const std::string& username, const std::string& otp_code, long otp_expiry_epoch){
  std::lock_guard<std::mutex> g(g_mu);
  auto it=g_users.find(username);
  if(it==g_users.end()) return false;
  it->second.otp_code=otp_code;
  it->second.otp_expiry_epoch=otp_expiry_epoch;
  it->second.otp_attempts=0;
  return true;
}
bool activate_registered_user(const std::string& username){
  std::lock_guard<std::mutex> g(g_mu);
  auto it=g_users.find(username);
  if(it==g_users.end()) return false;
  it->second.status="active";
  it->second.otp_code.clear();
  it->second.otp_expiry_epoch=0;
  return true;
}
bool bump_otp_attempts(const std::string& username){
  std::lock_guard<std::mutex> g(g_mu);
  auto it=g_users.find(username);
  if(it==g_users.end()) return false;
  it->second.otp_attempts++;
  return true;
}
bool update_user_password(const std::string& username, const std::string& password_hash){
  std::lock_guard<std::mutex> g(g_mu);
  auto it=g_users.find(username);
  if(it==g_users.end()) return false;
  it->second.password_hash=password_hash;
  it->second.otp_code.clear();
  it->second.otp_expiry_epoch=0;
  it->second.otp_attempts=0;
  return true;
}
bool delete_registered_user(const std::string& username){
  std::lock_guard<std::mutex> g(g_mu);
  return g_users.erase(username)>0;
}
std::string get_setting(const std::string& key, const std::string& def){ (void)key; return def; }

// ===================== mode produksi (PostgreSQL) =====================
#else

// Helper: buka RealPool dari env/config (kosong → PG tidak tersedia).
static bool open_pool(examvan::db::RealPool& out){
  auto cfg=Config::load();
  std::string db=cfg.database_url;
  if(db.empty()) if(auto* e=getenv("DATABASE_URL")) db=e;
  if(db.empty()) return false;
  std::string ci=pg_conninfo_from_url(db);
  if(ci.empty()) ci=db;
  out=examvan::db::RealPool(ci, 2);
  return out.connect();
}

static bool row_to_user(const PgResultPtr& r, int i, RegisteredUser& out){
  if(!r || PQresultStatus(r.get())!=PGRES_TUPLES_OK || PQntuples(r.get())<=i) return false;
  out.id=std::stoi(PQgetvalue(r.get(),i,0));
  out.username=PQgetvalue(r.get(),i,1);
  out.email=PQgetvalue(r.get(),i,2);
  out.password_hash=PQgetvalue(r.get(),i,3);
  out.status=PQgetvalue(r.get(),i,4);
  out.otp_code=PQgetvalue(r.get(),i,5);
  try{ out.otp_attempts=std::stoi(PQgetvalue(r.get(),i,6)); }catch(...){}
  try{ out.otp_expiry_epoch=std::stol(PQgetvalue(r.get(),i,7)); }catch(...){}
  return true;
}

static const char* kSelectUserSql =
  "SELECT id, username, COALESCE(email,''), password_hash, status, COALESCE(otp_code,''), "
  "COALESCE(otp_attempts,0), COALESCE(EXTRACT(EPOCH FROM otp_expiry)::bigint,0) "
  "FROM admin_users WHERE LOWER(username)=LOWER($1) LIMIT 1";

bool find_registered_user(const std::string& username, RegisteredUser& out){
#ifdef HAS_LIBPQ
  try{
    examvan::db::RealPool real;
    if(!open_pool(real)) return false;
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
    auto r=real.exec_params(c.get(), kSelectUserSql, {username});
    bool ok=row_to_user(r, 0, out);
    real.release(c.release());
    return ok;
  }catch(...){}
#endif
  return false;
}

bool find_registered_user_by_email(const std::string& email, RegisteredUser& out){
#ifdef HAS_LIBPQ
  try{
    examvan::db::RealPool real;
    if(!open_pool(real)) return false;
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
    auto r=real.exec_params(c.get(),
      "SELECT id, username, COALESCE(email,''), password_hash, status, COALESCE(otp_code,''), "
      "COALESCE(otp_attempts,0), COALESCE(EXTRACT(EPOCH FROM otp_expiry)::bigint,0) "
      "FROM admin_users WHERE LOWER(email)=LOWER($1) AND email <> '' LIMIT 1",{email});
    bool ok=row_to_user(r, 0, out);
    real.release(c.release());
    return ok;
  }catch(...){}
#endif
  return false;
}

int count_recent_registrations_by_ip(const std::string& ip){
  int n=0;
#ifdef HAS_LIBPQ
  try{
    examvan::db::RealPool real;
    if(!open_pool(real)) return 0;
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return 0;
    auto r=real.exec_params(c.get(),
      "SELECT COUNT(*) FROM admin_users WHERE registered_ip=$1 AND created_at > now() - interval '24 hours'",{ip});
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK && PQntuples(r.get())>0){
      try{ n=std::stoi(PQgetvalue(r.get(),0,0)); }catch(...){}
    }
    real.release(c.release());
  }catch(...){}
#endif
  return n;
}

bool insert_registered_user(const RegisteredUser& u){
#ifdef HAS_LIBPQ
  try{
    examvan::db::RealPool real;
    if(!open_pool(real)) return false;
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
    // Paritas Go CreateUser: kolom + kuota default dari saas_settings.
    const char* sql=
      "INSERT INTO admin_users (username,name,password_hash,status,instansi,role,"
      "max_exams,max_pdf_size,max_concurrent_exams,max_storage_size,whatsapp_number,email,"
      "expires_at,otp_code,otp_expiry,package,registered_ip,operator_created,created_by) "
      "VALUES ($1,'',$2,$3,'personal','[\"guru\"]',$4,$5,$6,$7,'',$8,"
      "now() + make_interval(days => $9),"
      "$10, CASE WHEN $11::bigint>0 THEN to_timestamp($11) ELSE NULL END,"
      "'free',$12,'false',0)";
    std::vector<std::string> params={
      u.username,            // $1
      u.password_hash,       // $2
      u.status,              // $3
      std::to_string(u.max_exams),                 // $4
      std::to_string(u.max_pdf_size),              // $5
      std::to_string(u.max_concurrent_exams),      // $6
      std::to_string(u.max_storage_size),          // $7
      u.email,               // $8
      std::to_string(u.active_days),               // $9
      u.otp_code,            // $10
      u.otp_expiry_epoch>0? std::to_string(u.otp_expiry_epoch):"0", // $11
      u.registered_ip,       // $12
    };
    auto r=real.exec_params(c.get(), sql, params);
    bool ok=r && PQresultStatus(r.get())==PGRES_TUPLES_OK;
    real.release(c.release());
    return ok;
  }catch(...){}
#endif
  return false;
}

bool update_user_otp(const std::string& username, const std::string& otp_code, long otp_expiry_epoch){
#ifdef HAS_LIBPQ
  try{
    examvan::db::RealPool real;
    if(!open_pool(real)) return false;
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
    auto r=real.exec_params(c.get(),
      "UPDATE admin_users SET otp_code=$1, otp_expiry=to_timestamp($2::bigint), otp_attempts=0 "
      "WHERE LOWER(username)=LOWER($3)",
      {otp_code, std::to_string(otp_expiry_epoch), username});
    bool ok=r && PQresultStatus(r.get())==PGRES_COMMAND_OK;
    real.release(c.release());
    return ok;
  }catch(...){}
#endif
  return false;
}

bool activate_registered_user(const std::string& username){
#ifdef HAS_LIBPQ
  try{
    examvan::db::RealPool real;
    if(!open_pool(real)) return false;
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
    auto r=real.exec_params(c.get(),
      "UPDATE admin_users SET status='active', otp_code=NULL, otp_expiry=NULL WHERE LOWER(username)=LOWER($1)",
      {username});
    bool ok=r && PQresultStatus(r.get())==PGRES_COMMAND_OK;
    real.release(c.release());
    return ok;
  }catch(...){}
#endif
  return false;
}

bool bump_otp_attempts(const std::string& username){
#ifdef HAS_LIBPQ
  try{
    examvan::db::RealPool real;
    if(!open_pool(real)) return false;
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
    auto r=real.exec_params(c.get(),
      "UPDATE admin_users SET otp_attempts = otp_attempts + 1 WHERE LOWER(username)=LOWER($1)",
      {username});
    bool ok=r && PQresultStatus(r.get())==PGRES_COMMAND_OK;
    real.release(c.release());
    return ok;
  }catch(...){}
#endif
  return false;
}

bool update_user_password(const std::string& username, const std::string& password_hash){
#ifdef HAS_LIBPQ
  try{
    examvan::db::RealPool real;
    if(!open_pool(real)) return false;
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
    auto r=real.exec_params(c.get(),
      "UPDATE admin_users SET password_hash=$1, otp_code=NULL, otp_expiry=NULL, otp_attempts=0 "
      "WHERE LOWER(username)=LOWER($2)",
      {password_hash, username});
    bool ok=r && PQresultStatus(r.get())==PGRES_COMMAND_OK;
    real.release(c.release());
    return ok;
  }catch(...){}
#endif
  return false;
}

bool delete_registered_user(const std::string& username){
#ifdef HAS_LIBPQ
  try{
    examvan::db::RealPool real;
    if(!open_pool(real)) return false;
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
    auto r=real.exec_params(c.get(),
      "DELETE FROM admin_users WHERE LOWER(username)=LOWER($1)", {username});
    bool ok=r && PQresultStatus(r.get())==PGRES_COMMAND_OK;
    real.release(c.release());
    return ok;
  }catch(...){}
#endif
  return false;
}

std::string get_setting(const std::string& key, const std::string& def){
#ifdef HAS_LIBPQ
  try{
    examvan::db::RealPool real;
    if(!open_pool(real)) return def;
    auto c=real.acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return def;
    auto r=real.exec_params(c.get(), "SELECT value FROM saas_settings WHERE key=$1", {key});
    std::string out=def;
    if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK && PQntuples(r.get())>0){
      out=PQgetvalue(r.get(),0,0);
    }
    real.release(c.release());
    return out;
  }catch(...){}
#endif
  return def;
}

#endif // EXAMVAN_TESTING

} // namespace examvan::handlers::auth