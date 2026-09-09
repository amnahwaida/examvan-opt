// Pass-24 TDD — tindak lanjut terbuka pass-21 (addendum): heartbeat flusher
// masih membangun pool PG baru per tick-30s (`DbPool` + `RealPool(...,60)`
// stack-lokal di drain_heartbeats_once) → churn koneksi TCP+auth PG 2×/menit
// seumur proses. Kontrak: flusher wajib memakai pool proses-wide yang sama
// dengan handler (db/pool_global.hpp) — koneksi di-reuse lintas tick.
//
// Kontrak (RED dulu, GREEN setelah remediasi):
//  1. P32.FlusherUsesGlobalPool      — drain_heartbeats_once memanggil
//     db::global_pool (bukan membangun RealPool stack-lokal per tick).
//  2. P32.FlusherKeepsConnectionsHot — sumber tidak lagi membuat
//     `DbPool pool(cfg.database_url, 60)` + `RealPool real(...,60)` per tick.
//  3. P32.NoDetachInQueueSources     — anti-regresi C5 (zero std::thread
//     detach di jalur server/queue).
//
// Kontrak infrastruktur murni (skema DB, Redis, E2E handler) ada di
// tests/test_pg_integration_tdd.cpp — SKIP otomatis tanpa DATABASE_URL.
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifdef HAS_HIREDIS
#include "redis/client.hpp"
#include "redis/redis_real.hpp"
#include <hiredis/hiredis.h>
#endif

namespace {

std::string read_src32(const std::string& p) {
  std::ifstream f(p);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string between(const std::string& src, const std::string& begin_marker,
                    const std::string& end_marker) {
  auto b = src.find(begin_marker);
  if (b == std::string::npos) return "";
  auto e = src.find(end_marker, b);
  return e == std::string::npos ? src.substr(b) : src.substr(b, e - b);
}

}  // namespace

TEST(P32, FlusherUsesGlobalPool) {
  auto src = read_src32("src/queue/submission_queue.cpp");
  ASSERT_FALSE(src.empty()) << "src/queue/submission_queue.cpp harus ada";
  auto seg = between(src, "int drain_heartbeats_once()", "HeartbeatFlusher::HeartbeatFlusher");
  ASSERT_FALSE(seg.empty()) << "drain_heartbeats_once() tidak ditemukan";
  EXPECT_NE(seg.find("global_pool"), std::string::npos)
      << "heartbeat flusher wajib pakai pool PG proses-wide (db/pool_global.hpp) — "
         "bukan pool baru per tick-30s (churn koneksi)";
}

TEST(P32, FlusherKeepsConnectionsHot) {
  auto src = read_src32("src/queue/submission_queue.cpp");
  ASSERT_FALSE(src.empty());
  auto seg = between(src, "int drain_heartbeats_once()", "HeartbeatFlusher::HeartbeatFlusher");
  ASSERT_FALSE(seg.empty());
  EXPECT_EQ(seg.find("RealPool"), std::string::npos)
      << "RealPool stack-lokal per tick dilarang — pakai global_pool()";
  EXPECT_EQ(seg.find("DbPool"), std::string::npos)
      << "DbPool wrapper per tick dilarang (tidak dipakai lagi di jalur ini)";
}

TEST(P32, NoDetachInQueueSources) {
  for (const char* f : {"src/queue/submission_queue.cpp", "src/server/server.cpp",
                        "src/main.cpp"}) {
    auto src = read_src32(f);
    ASSERT_FALSE(src.empty()) << f << " harus ada";
    EXPECT_EQ(src.find(".detach()"), std::string::npos)
        << f << " tidak boleh detach thread (anti-regresi C5)";
  }
}

// P32 (temuan baru): release_job hanya menghapus lock dari peta in-process —
// key Redis (TTL 3600 dtk untuk "expiry") tidak pernah di-DEL. Akibatnya
// run_expiry_job PERTAMA memblokir semua pemanggilan berikutnya (proses sama
// maupun rerun CI yang memakai Redis shared) sampai TTL habis: test expiry
// bergantung urutan & rerun berflak. Kontrak: release_job wajib menghapus
// key Redis sehingga lock benar-benar dilepas.
#ifdef HAS_HIREDIS

TEST(P32, ReleaseJobDeletesRedisLockKey) {
  const char* u = getenv("REDIS_URL");
  if (!u || !*u) GTEST_SKIP() << "REDIS_URL tidak diset";
  auto ctx = examvan::redis_real::connect_redis(u);
  if (!ctx) GTEST_SKIP() << "Redis tidak terjangkau";

  examvan::RedisClient client(u);
  const std::string key = client.prefixed("job:p32releaselock");
  auto* del = reinterpret_cast<redisReply*>(redisCommand(ctx.get(), "DEL %s", key.c_str()));
  if (del) freeReplyObject(del);

  ASSERT_TRUE(client.try_acquire_job("p32releaselock", 60));
  auto* get1 = reinterpret_cast<redisReply*>(redisCommand(ctx.get(), "GET %s", key.c_str()));
  ASSERT_TRUE(get1 && get1->type == REDIS_REPLY_STRING)
      << "lock harus ada di Redis setelah acquire";
  freeReplyObject(get1);

  client.release_job("p32releaselock");
  auto* get2 = reinterpret_cast<redisReply*>(redisCommand(ctx.get(), "GET %s", key.c_str()));
  ASSERT_TRUE(get2);
  EXPECT_EQ(get2->type, REDIS_REPLY_NIL)
      << "release_job wajib menghapus key lock dari Redis (bukan peta memori saja)";
  freeReplyObject(get2);
}

#endif  // HAS_HIREDIS
