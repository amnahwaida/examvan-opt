#include "redis/client.hpp"
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
