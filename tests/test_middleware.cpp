#include <gtest/gtest.h>
#include "middleware/version.hpp"
#include "middleware/ratelimit.hpp"
#include "middleware/cors.hpp"
#include "middleware/body_limit.hpp"
using namespace examvan::middleware;

TEST(Middleware, VersionCompare) {
  EXPECT_EQ(compare_versions("2.7.2","2.7.2"),0);
  EXPECT_LT(compare_versions("2.7.1","2.7.2"),0);
  EXPECT_GT(compare_versions("2.8.0","2.7.2"),0);
  EXPECT_TRUE(is_version_allowed("2.7.2","2.7.2"));
  EXPECT_FALSE(is_version_allowed("2.7.1","2.7.2"));
  EXPECT_TRUE(is_version_allowed("",""));
}

TEST(Middleware, VersionGate426) {
  examvan::Request req; req.headers["X-App-Version"]="1.0.0";
  auto res=version_gate(req,"2.7.2",[](auto){ examvan::Response r; r.status=200; return r; });
  EXPECT_EQ(res.status,426);
}

/* Semantik AndroidVersionCheck Go (version.go) — sumber: F8 shadow parity */
TEST(Middleware, ShouldBlockGoSemantics) {
  // 1) client kosong → izinkan (client web)
  EXPECT_FALSE(should_block_version("", "2.7.2"));
  // 2) required kosong → izinkan (belum ada APK terbit)
  EXPECT_FALSE(should_block_version("1.0.0", ""));
  // 3a) client tua + required ada → blok
  EXPECT_TRUE(should_block_version("1.0.0", "2.7.2"));
  // 3b) client sama/lebih baru → izinkan
  EXPECT_FALSE(should_block_version("2.7.2", "2.7.2"));
  EXPECT_FALSE(should_block_version("2.8.0", "2.7.2"));
}

TEST(Middleware, RateLimit) {
  RateLimiter rl(2, std::chrono::seconds(10));
  EXPECT_TRUE(rl.allow("ip1"));
  EXPECT_TRUE(rl.allow("ip1"));
  EXPECT_FALSE(rl.allow("ip1"));
  EXPECT_TRUE(rl.allow("ip2"));
  rl.reset();
  EXPECT_TRUE(rl.allow("ip1"));
}

TEST(Middleware, CorsAllowAll) {
  EXPECT_FALSE(is_origin_allowed("https://a.com",""));
  EXPECT_TRUE(is_origin_allowed("https://a.com","https://a.com, https://b.com"));
  EXPECT_FALSE(is_origin_allowed("https://evil.com","https://a.com"));
}

TEST(Middleware, BodyLimit) {
  examvan::Request req; req.body=std::string(100,'x');
  auto ok=body_limit(req, 50, [](auto){ examvan::Response r; r.status=200; return r; });
  EXPECT_EQ(ok.status,413);
  auto ok2=body_limit(req, 200, [](auto){ examvan::Response r; r.status=200; return r; });
  EXPECT_EQ(ok2.status,200);
}
