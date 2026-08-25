#include "middleware/ratelimit.hpp"
namespace examvan::middleware {
bool RateLimiter::allow(const std::string& key){
  std::lock_guard<std::mutex> g(mu_);
  auto now=std::chrono::steady_clock::now();
  auto it=buckets_.find(key);
  if(it==buckets_.end()|| now - it->second.second >= window_){
    buckets_[key]={1, now}; return true;
  }
  if(it->second.first < max_){ it->second.first++; return true; }
  return false;
}
void RateLimiter::reset(){ std::lock_guard<std::mutex> g(mu_); buckets_.clear(); }
} // namespace examvan::middleware
