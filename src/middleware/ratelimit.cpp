#include "middleware/ratelimit.hpp"
namespace examvan::middleware {
bool RateLimiter::allow(const std::string& key){
  std::lock_guard<std::mutex> g(mu_);
  auto now=std::chrono::steady_clock::now();
  auto it=buckets_.find(key);
  if(it==buckets_.end()|| now - it->second.second >= window_){
    buckets_[key]={1, now};
    // Evict malas: saat map membesar (IP unik/spoofed baru), sapu entri
    // yang window-nya sudah lewat agar memori tetap O(aktif) — tanpa ini,
    // banjir X-Real-IP unik menumbuhkan buckets_ tanpa batas (DoS memori).
    if(buckets_.size() > 1024 && (buckets_.size() & 63)==0){
      for(auto i=buckets_.begin(); i!=buckets_.end();){
        if(now - i->second.second >= window_) i=buckets_.erase(i);
        else ++i;
      }
    }
    return true;
  }
  if(it->second.first < max_){ it->second.first++; return true; }
  return false;
}
void RateLimiter::reset(){ std::lock_guard<std::mutex> g(mu_); buckets_.clear(); }
} // namespace examvan::middleware
