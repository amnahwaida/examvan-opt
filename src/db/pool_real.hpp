#pragma once
#include "db/pool.hpp"
#ifdef HAS_LIBPQ
#include <libpq-fe.h>
#include <memory>
#include <vector>
#include <mutex>

namespace examvan::db {

struct PgConnDeleter { void operator()(PGconn* c) const { if(c) PQfinish(c); } };
using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;

struct PgResultDeleter { void operator()(PGresult* r) const { if(r) PQclear(r); } };
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

class RealPool {
public:
  explicit RealPool(const std::string& conninfo, int max_conns=60);
  bool connect();
  PgConnPtr acquire();
  void release(PGconn* c);
  bool ping();
  PgResultPtr exec_params(PGconn* c, const std::string& sql, const std::vector<std::string>& params);
private:
  std::string conninfo_;
  int max_conns_;
  std::vector<PGconn*> idle_;
  std::mutex mu_;
};

} // namespace examvan::db
#endif
