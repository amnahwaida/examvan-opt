#include "redis/client.hpp"
#include <unordered_set>
#include <mutex>
namespace examvan {
bool RedisClient::connect(){
  if(url.empty()) return false;
  connected = url.rfind("redis://",0)==0;
  return connected;
}
static std::unordered_set<std::string> g_locks;
static std::mutex g_mu;
bool RedisClient::try_acquire_job(const std::string& job, int ttl){
  (void)ttl;
  std::lock_guard<std::mutex> g(g_mu);
  std::string k=prefixed("job:"+job);
  if(g_locks.count(k)) return false;
  g_locks.insert(k); return true;
}
void RedisClient::release_job(const std::string& job){
  std::lock_guard<std::mutex> g(g_mu);
  g_locks.erase(prefixed("job:"+job));
}
}  // namespace examvan
