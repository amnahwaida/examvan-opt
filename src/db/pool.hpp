#pragma once
#include <string>
#include <optional>

namespace examvan {

struct DbPool {
  std::string url;
  int max_conns{60};
  bool connected{false};
  explicit DbPool(std::string u, int m): url(std::move(u)), max_conns(m) {}
  bool connect();
  bool ping() const { return connected; }
  void disconnect() { connected=false; }
  std::string sanitized_url() const;
  bool has_valid_url() const;
};

struct DbTx {
  DbPool* pool{nullptr};
  bool committed{false};
  explicit DbTx(DbPool* p): pool(p) {}
  ~DbTx(){ if(!committed && pool && pool->connected) {/* rollback */} }
  void commit(){ committed=true; }
  void rollback(){ committed=false; }
};

std::string pg_conninfo_from_url(const std::string& url);

}  // namespace examvan
