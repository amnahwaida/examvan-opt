#include "redis/client.hpp"
#ifdef HAS_HIREDIS
#include "redis/redis_real.hpp"
#endif
#include <unordered_map>
#include <mutex>
#include <chrono>
namespace examvan {
bool RedisClient::connect(){
  if(url.empty()) return false;
  connected = url.rfind("redis://",0)==0;
  return connected;
}
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_locks;
static std::mutex g_mu;
bool RedisClient::try_acquire_job(const std::string& job, int ttl){
#ifdef HAS_HIREDIS
  // P18-H6: multi-replika wajib SETNX terdistribusi; fallback in-process
  // hanya bila Redis tak terjangkau (single-node / test).
  try{
    if(!url.empty()){
      auto ctx=redis_real::connect_redis(url);
      if(ctx){
        bool ok=redis_real::redis_setnx(ctx.get(), prefixed("job:"+job), "1", ttl>0?ttl:60);
        if(ok) return true;
        // SETNX gagal = lock dipegang replika lain; jangan fallback in-memory
        // (akan double-run). Cek ringan: bila key memang ada → tolak.
        std::string v=redis_real::redis_get(ctx.get(), prefixed("job:"+job));
        if(!v.empty()) return false;
      }
    }
  }catch(...){}
#endif
  auto now=std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> g(g_mu);
  std::string k=prefixed("job:"+job);
  auto it=g_locks.find(k);
  if(it!=g_locks.end()){
    if(now < it->second) return false;
    g_locks.erase(it);
  }
  g_locks[k]=now + std::chrono::seconds(ttl>0?ttl:60);
  return true;
}
void RedisClient::release_job(const std::string& job){
  std::lock_guard<std::mutex> g(g_mu);
  g_locks.erase(prefixed("job:"+job));
}
}  // namespace examvan
