#include <gtest/gtest.h>
#include "session/cookie.hpp"
#include "db/pool.hpp"
#include "redis/client.hpp"
#include "middleware/turnstile.hpp"
#include "http/router_full.hpp"
#include "config/config.hpp"
using namespace examvan;

TEST(CookieDual, Rotation) {
  std::string cur="cur-secret-1234567890abcdef";
  std::string prev="prev-secret-1234567890abcde";
  std::string payload=b64_encode("admin_id=1&username=a");
  auto cv=encode_cookie_value(prev, payload);
  auto d=verify_session_cookie_dual(cur, prev, cv);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->admin_id, 1);
  auto d2=verify_session_cookie(cur, cv);
  EXPECT_FALSE(d2.has_value());
}

TEST(CookieDual, B64Url) {
  std::string s="hello+world/test";
  EXPECT_EQ(b64url_decode(b64url_encode(s)), s);
}

TEST(DbPool, Sanitized) {
  DbPool p("postgresql://user:pass@db:5432/examvan", 60);
  EXPECT_TRUE(p.has_valid_url());
  EXPECT_NE(p.sanitized_url().find("***"), std::string::npos);
  EXPECT_FALSE(p.sanitized_url().find("pass") != std::string::npos);
  DbPool bad("mysql://x",60);
  EXPECT_FALSE(bad.has_valid_url());
}

TEST(RedisPrefix, Isolated) {
  RedisClient r("redis://redis:6379/0","test-prefix");
  EXPECT_EQ(r.prefixed("job:expiry"), "test-prefix:job:expiry");
  EXPECT_TRUE(r.try_acquire_job("expiry"));
  EXPECT_FALSE(r.try_acquire_job("expiry"));
  r.release_job("expiry");
  EXPECT_TRUE(r.try_acquire_job("expiry"));
  r.release_job("expiry");
}

TEST(Turnstile, Bypass) {
  EXPECT_TRUE(middleware::verify_turnstile("test-bypass-token","sec","1.1.1.1"));
  EXPECT_FALSE(middleware::verify_turnstile("","sec","1.1.1.1"));
}

TEST(FullRouter, Comprehensive) {
  Config cfg; Router r; register_full_routes(r,cfg);
  EXPECT_GE(r.routes().size(), 60u);
  Request req; req.method="GET"; req.path="/admin/api/pengawas/exams";
  EXPECT_EQ(r.dispatch(req).status,200);
  req.path="/api/webhook"; req.method="POST"; req.body="{}";
  EXPECT_EQ(r.dispatch(req).status,200);
  req.path="/admin/api/queue/status"; req.method="GET";
  EXPECT_EQ(r.dispatch(req).status,200);
}
