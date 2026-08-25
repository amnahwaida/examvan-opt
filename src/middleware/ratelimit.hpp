#pragma once
#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>

namespace examvan::middleware {

class RateLimiter {
public:
  RateLimiter(int max_req, std::chrono::seconds window): max_(max_req), window_(window) {}
  bool allow(const std::string& key);
  void reset();
private:
  int max_;
  std::chrono::seconds window_;
  std::mutex mu_;
  std::unordered_map<std::string, std::pair<int, std::chrono::steady_clock::time_point>> buckets_;
};

} // namespace examvan::middleware
