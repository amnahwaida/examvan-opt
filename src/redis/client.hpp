#pragma once
#include <string>
#include <optional>
namespace examvan {
struct RedisClient {
  std::string url;
  std::string prefix;
  bool connected{false};
  explicit RedisClient(std::string u, std::string pfx=""): url(std::move(u)), prefix(std::move(pfx)) {}
  bool connect();
  bool ping() const { return connected; }
  std::string prefixed(const std::string& key) const {
    if(prefix.empty()) return key;
    return prefix + ":" + key;
  }
  bool try_acquire_job(const std::string& job_name, int ttl_seconds=60);
  void release_job(const std::string& job_name);
};
}  // namespace examvan
