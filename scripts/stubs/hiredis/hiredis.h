// Minimal stub of hiredis.h — syntax-check only, NOT a real implementation.
// Dipakai scripts/check-docker-paths.sh untuk men-compile jalur
// #ifdef HAS_HIREDIS di mesin dev tanpa libhiredis-dev.
#pragma once
#include <stddef.h>
#include <sys/time.h>

#define REDIS_REPLY_STRING 1
#define REDIS_REPLY_ARRAY 2
#define REDIS_REPLY_INTEGER 3
#define REDIS_REPLY_NIL 4
#define REDIS_REPLY_STATUS 5
#define REDIS_REPLY_ERROR 6

struct redisReply {
  int type;
  long long integer;
  size_t len;
  char* str;
  size_t elements;
  struct redisReply** element;
};

typedef struct redisContext {
  int err;
  char errstr[128];
  int fd;
  int flags;
  char* obuf;
  unsigned int reader_state;
} redisContext;

void* redisCommand(redisContext* c, const char* format, ...);
void* redisCommandArgv(redisContext* c, int argc, const char** argv, const size_t* argvlen);
void freeReplyObject(void* reply);
void redisFree(redisContext* c);
redisContext* redisConnect(const char* ip, int port);
redisContext* redisConnectWithTimeout(const char* ip, int port, const struct timeval tv);
int redisSetTimeout(redisContext* c, const struct timeval tv);
int redisAppendCommand(redisContext* c, const char* format, ...);
int redisGetReply(redisContext* c, void** reply);
