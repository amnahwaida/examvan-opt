#pragma once
#include "db/pool.hpp"
#include <optional>
#ifdef HAS_LIBPQ
#include <libpq-fe.h>
#include <memory>
#include <vector>
#include <mutex>
#include <utility>

namespace examvan::db {

struct PgConnDeleter { void operator()(PGconn* c) const { if(c) PQfinish(c); } };
using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;

struct PgResultDeleter { void operator()(PGresult* r) const { if(r) PQclear(r); } };
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

class RealPool {
public:
  // Default ctor: pool belum ter-konfigurasi; open_pool() meng-assign
  // RealPool(conninfo, n) sebelum dipakai. (auth_store memakai pola
  // `RealPool real; if(!open_pool(real)) ...`.)
  RealPool() = default;
  explicit RealPool(const std::string& conninfo, int max_conns=60);
  // Move: anggota std::mutex membuat move default ter-delete, padahal
  // open_pool() meng-assign RealPool(ci,n) ke objek default.
  RealPool(RealPool&& o) noexcept
    : conninfo_(std::move(o.conninfo_)), max_conns_(o.max_conns_),
      idle_(std::move(o.idle_)) { o.max_conns_=0; }
  RealPool& operator=(RealPool&& o) noexcept {
    if(this!=&o){
      conninfo_=std::move(o.conninfo_); max_conns_=o.max_conns_;
      idle_=std::move(o.idle_); o.max_conns_=0;
    }
    return *this;
  }
  RealPool(const RealPool&) = delete;
  RealPool& operator=(const RealPool&) = delete;
  bool connect();
  PgConnPtr acquire();
  void release(PGconn* c);
  bool ping();
  PgResultPtr exec_params(PGconn* c, const std::string& sql, const std::vector<std::string>& params);
  PgResultPtr exec_params_nullable(PGconn* c, const std::string& sql, const std::vector<std::optional<std::string>>& params);
  // Execute parameterized SQL while returning the connection to the pool.
  PgResultPtr exec_params_pooled(const std::string& sql, const std::vector<std::string>& params);
private:
  std::string conninfo_;
  int max_conns_{60};
  std::vector<PGconn*> idle_;
  std::mutex mu_;
};

} // namespace examvan::db
#endif
