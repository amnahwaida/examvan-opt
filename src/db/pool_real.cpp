#include "db/pool_real.hpp"
#ifdef HAS_LIBPQ
#include <libpq-fe.h>

namespace examvan::db {

RealPool::RealPool(const std::string& ci, int max_c): conninfo_(ci), max_conns_(max_c) {}

RealPool::~RealPool(){
  for(auto* c: idle_) if(c) PQfinish(c);
  idle_.clear();
}

bool RealPool::connect(){
  auto* c = PQconnectdb(conninfo_.c_str());
  if(PQstatus(c)!=CONNECTION_OK){ PQfinish(c); return false; }
  std::lock_guard<std::mutex> g(mu_);
  if((int)idle_.size()>=max_conns_){ PQfinish(c); return true; }
  idle_.push_back(c);
  return true;
}

bool RealPool::ping(){
  std::lock_guard<std::mutex> g(mu_);
  if(idle_.empty()) return false;
  // P18-M12: ping stale (cek status holder lama) → validasi aktif via SELECT 1.
  auto* c=idle_.front();
  if(PQstatus(c)!=CONNECTION_OK) return false;
  auto* r=PQexec(c,"SELECT 1");
  bool ok=r && PQresultStatus(r)==PGRES_TUPLES_OK;
  if(r) PQclear(r);
  return ok;
}

PgConnPtr RealPool::new_connection(){
  /* P34-G3: PQconnectdb (TCP+auth, bisa detik-an saat PG lambat/restart)
   * TIDAK BOLEH di bawah mutex — sebelumnya seluruh pool (termasuk thread
   * pemakai & pelepas koneksi) membeku mengikuti satu koneksi lambat.
   * Didefinisikan SEBELUM acquire() dan dipanggil SETELAH mutex dilepas —
   * kontrak test memeriksa bahwa window acquire→release tidak memanggil
   * PQconnectdb. connect_timeout (default di sini bila conninfo belum
   * membawanya) membatasi durasi TCP+auth; tanpa itu satu koneksi lambat
   * bisa menggantung thread pemanggil tanpa batas. */
  std::string ci=conninfo_;
  if(ci.find("connect_timeout=")==std::string::npos) ci+=" connect_timeout=5";
  auto* c=PQconnectdb(ci.c_str());
  return PgConnPtr(c);
}

PgConnPtr RealPool::acquire(){
  /* P33-G3: pola lock → ambil idle → unlock → sambung baru DI LUAR lock →
   * return (via new_connection di atas). */
  {
    std::lock_guard<std::mutex> g(mu_);
    if(!idle_.empty()){
      auto* c=idle_.back(); idle_.pop_back();
      return PgConnPtr(c);
    }
  }
  return new_connection();
}

void RealPool::release(PGconn* c){
  if(!c||PQstatus(c)!=CONNECTION_OK){ if(c) PQfinish(c); return; }
  std::lock_guard<std::mutex> g(mu_);
  if((int)idle_.size()<max_conns_) idle_.push_back(c);
  else PQfinish(c);
}

PgResultPtr RealPool::exec_params(PGconn* c, const std::string& sql, const std::vector<std::string>& params){
  std::vector<const char*> vals;
  for(auto& p: params) vals.push_back(p.c_str());
  PGresult* r=PQexecParams(c, sql.c_str(), static_cast<int>(vals.size()), nullptr, vals.data(), nullptr, nullptr, 0);
  return PgResultPtr(r);
}

PgResultPtr RealPool::exec_params_nullable(PGconn* c, const std::string& sql, const std::vector<std::optional<std::string>>& params){
  std::vector<const char*> vals;
  vals.reserve(params.size());
  for(const auto& p: params) vals.push_back(p ? p->c_str() : nullptr);
  PGresult* r=PQexecParams(c, sql.c_str(), static_cast<int>(vals.size()), nullptr, vals.data(), nullptr, nullptr, 0);
  return PgResultPtr(r);
}

PgResultPtr RealPool::exec_params_pooled(const std::string& sql, const std::vector<std::string>& params){
  auto c=acquire();
  if(!c || PQstatus(c.get())!=CONNECTION_OK) return {};
  auto result=exec_params(c.get(),sql,params);
  release(c.release());
  return result;
}

} // namespace examvan::db
#endif
