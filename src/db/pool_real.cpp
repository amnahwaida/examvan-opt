#include "db/pool_real.hpp"
#ifdef HAS_LIBPQ
#include <libpq-fe.h>

namespace examvan::db {

RealPool::RealPool(const std::string& ci, int max_c): conninfo_(ci), max_conns_(max_c) {}

bool RealPool::connect(){
  auto* c = PQconnectdb(conninfo_.c_str());
  if(PQstatus(c)!=CONNECTION_OK){ PQfinish(c); return false; }
  idle_.push_back(c);
  return true;
}

bool RealPool::ping(){
  if(idle_.empty()) return false;
  return PQstatus(idle_.front())==CONNECTION_OK;
}

PgConnPtr RealPool::acquire(){
  std::lock_guard<std::mutex> g(mu_);
  if(!idle_.empty()){
    auto* c=idle_.back(); idle_.pop_back();
    return PgConnPtr(c);
  }
  auto* c=PQconnectdb(conninfo_.c_str());
  return PgConnPtr(c);
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
