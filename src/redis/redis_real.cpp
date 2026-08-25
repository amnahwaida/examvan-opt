#include "redis/redis_real.hpp"
#ifdef HAS_HIREDIS
#include <hiredis/hiredis.h>
namespace examvan::redis_real {
RedisPtr connect_redis(const std::string& url){
  // url redis://host:port/0 -> parse host port
  std::string host="127.0.0.1"; int port=6379;
  auto p=url.find("://"); std::string rest=p==std::string::npos?url:url.substr(p+3);
  auto colon=rest.find(':'); auto slash=rest.find('/');
  if(colon!=std::string::npos) host=rest.substr(0,colon);
  if(colon!=std::string::npos) port=std::stoi(rest.substr(colon+1, slash-colon-1));
  auto* c=redisConnect(host.c_str(), port);
  if(!c||c->err) { if(c) redisFree(c); return nullptr; }
  return RedisPtr(c);
}
bool redis_ping(redisContext* c){ auto* r=(redisReply*)redisCommand(c,"PING"); if(!r) return false; bool ok=r->type==REDIS_REPLY_STATUS; freeReplyObject(r); return ok; }
bool redis_set(redisContext* c, const std::string& k, const std::string& v, int ttl){
  auto* r=(redisReply*)redisCommand(c,"SET %s %b EX %d",k.c_str(),v.data(),v.size(),ttl); if(!r) return false; bool ok=r->type!=REDIS_REPLY_ERROR; freeReplyObject(r); return ok;
}
bool redis_setnx(redisContext* c, const std::string& k, const std::string& v, int ttl){
  auto* r=(redisReply*)redisCommand(c,"SET %s %b NX EX %d",k.c_str(),v.data(),v.size(),ttl); if(!r) return false; bool ok=r->type==REDIS_REPLY_STATUS; freeReplyObject(r); return ok;
}
std::string redis_get(redisContext* c, const std::string& k){
  auto* r=(redisReply*)redisCommand(c,"GET %s",k.c_str()); if(!r||r->type!=REDIS_REPLY_STRING){ if(r) freeReplyObject(r); return ""; } std::string s(r->str,r->len); freeReplyObject(r); return s;
}
} // namespace examvan::redis_real
#endif
