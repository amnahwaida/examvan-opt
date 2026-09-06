#include "redis/redis_real.hpp"
#ifdef HAS_HIREDIS
#include <hiredis/hiredis.h>
namespace examvan::redis_real {
RedisPtr connect_redis(const std::string& url){
  // P18-M14: parse defensif — URL rusak / port non-numerik / tanpa skema
  // tidak boleh throw; kembalikan nullptr agar caller fail-closed.
  try{
  std::string host="127.0.0.1"; int port=6379;
  std::string password; int db=0;
  if(url.empty()) return nullptr;
  auto p=url.find("://"); std::string rest=p==std::string::npos?url:url.substr(p+3);
  // P18-M14: userinfo redis://:pass@host:port/db — ekstrak password.
  auto at=rest.find('@');
  if(at!=std::string::npos){
    std::string userinfo=rest.substr(0,at);
    auto cp=userinfo.find(':');
    if(cp!=std::string::npos) password=userinfo.substr(cp+1);
    else if(!userinfo.empty()) password=userinfo;
    rest=rest.substr(at+1);
  }
  auto slash=rest.find('/'); std::string hostport=slash==std::string::npos?rest:rest.substr(0,slash);
  if(slash!=std::string::npos){
    std::string dbs=rest.substr(slash+1);
    auto q=dbs.find('?'); if(q!=std::string::npos) dbs=dbs.substr(0,q);
    if(!dbs.empty()){
      bool alldigit=true;
      for(char c: dbs) if(!isdigit((unsigned char)c)) alldigit=false;
      if(alldigit){ try{ db=std::stoi(dbs); }catch(...){ db=0; } }
    }
  }
  auto colon=hostport.find(':');
  if(colon!=std::string::npos){
    host=hostport.substr(0,colon);
    std::string ports=hostport.substr(colon+1);
    if(ports.empty()||ports.size()>5) return nullptr;
    for(char c: ports) if(!isdigit((unsigned char)c)) return nullptr;
    try{ port=std::stoi(ports); }catch(...){ return nullptr; }
    if(port<=0||port>65535) return nullptr;
  } else if(!hostport.empty()) host=hostport;
  if(host.empty()) return nullptr;
  auto* c=redisConnect(host.c_str(), port);
  if(!c||c->err) { if(c) redisFree(c); return nullptr; }
  RedisPtr ptr(c);
  if(!password.empty()){
    auto* r=(redisReply*)redisCommand(ptr.get(),"AUTH %s",password.c_str());
    if(!r){ return nullptr; }
    bool ok=r->type==REDIS_REPLY_STATUS;
    freeReplyObject(r);
    if(!ok) return nullptr;
  }
  if(db!=0){
    auto* r=(redisReply*)redisCommand(ptr.get(),"SELECT %d",db);
    if(r) freeReplyObject(r);
  }
  return ptr;
  }catch(...){ return nullptr; }
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
long long redis_llen(redisContext* c, const std::string& k){
  auto* r=(redisReply*)redisCommand(c,"LLEN %s",k.c_str()); if(!r||r->type!=REDIS_REPLY_INTEGER){ if(r) freeReplyObject(r); return 0; } long long n=r->integer; freeReplyObject(r); return n;
}
} // namespace examvan::redis_real
#endif
