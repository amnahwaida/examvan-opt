#pragma once
#include "redis/client.hpp"
#ifdef HAS_HIREDIS
#include <hiredis/hiredis.h>
#include <memory>

namespace examvan::redis_real {

struct RedisDeleter{ void operator()(redisContext* c) const { if(c) redisFree(c); } };
using RedisPtr = std::unique_ptr<redisContext, RedisDeleter>;

RedisPtr connect_redis(const std::string& url);
bool redis_ping(redisContext* c);
bool redis_set(redisContext* c, const std::string& key, const std::string& val, int ttl_sec=300);
bool redis_setnx(redisContext* c, const std::string& key, const std::string& val, int ttl_sec=60);
std::string redis_get(redisContext* c, const std::string& key);

} // namespace examvan::redis_real
#endif
