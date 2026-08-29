#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include "config/config.hpp"
#include "db/pool.hpp"

static std::string read_file(const std::string& p){
  std::ifstream f(p); if(!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)), {});
}

TEST(P3_Server, PosixShouldHaveETagNotImmutableOnly) {
  auto c = read_file("src/server/server.cpp");
  EXPECT_NE(c.find("ETag"), std::string::npos) << "static file handler must emit ETag";
  EXPECT_NE(c.find("Keep-Alive"), std::string::npos) << "should support keep-alive, not always close";
}

TEST(P3_Server, PosixShouldSupportLargeWsFrame) {
  auto c = read_file("src/server/server.cpp");
  EXPECT_EQ(c.find("len == 127) { break;"), std::string::npos) << "127 frame length should not just break (support 64-bit)";
  EXPECT_NE(c.find("126"), std::string::npos);
}

TEST(P3_Server, UwsShouldPersistClientAcrossMessages) {
  auto c = read_file("src/server/server.cpp");
  EXPECT_NE(c.find("WsData"), std::string::npos);
  size_t open = c.find(".open");
  size_t msg = c.find(".message");
  ASSERT_NE(open, std::string::npos); ASSERT_NE(msg, std::string::npos);
  std::string segment = c.substr(open, msg-open);
  EXPECT_NE(segment.find("shared_ptr"), std::string::npos) << "open should store client in WsData";
  EXPECT_NE(segment.find("client"), std::string::npos);
  std::string msg_seg = c.substr(msg, 800);
  EXPECT_EQ(msg_seg.find("make_shared<examvan::Client>()"), std::string::npos) << "message should NOT create ephemeral Client, should reuse WsData client";
  EXPECT_NE(msg_seg.find("d->client"), std::string::npos) << "message should reuse d->client";
}

TEST(P3_Server, MainShouldWireRealPoolAndRedis) {
  auto m = read_file("src/main.cpp");
  EXPECT_NE(m.find("RealPool"), std::string::npos) << "main.cpp should use db::RealPool when HAS_LIBPQ";
  EXPECT_NE(m.find("redis_real"), std::string::npos) << "main.cpp should wire redis_real when HAS_HIREDIS";
  EXPECT_EQ(m.find("host=db dbname=examvan"), std::string::npos) << "should not hardcode conninfo";
}

TEST(P3_Db, PoolRealShouldEnforceMaxConns) {
  auto c = read_file("src/db/pool_real.cpp");
  EXPECT_NE(c.find("max_conns"), std::string::npos);
  auto pool = read_file("src/db/pool.cpp");
  EXPECT_NE(pool.find("pg_conninfo_from_url"), std::string::npos);
  EXPECT_NE(read_file("src/config/config.cpp").find("DATABASE_MAX_CONNS"), std::string::npos);
}

TEST(P3_Security, CookieShouldHaveSecureAndMaxAge) {
  auto c = read_file("src/handlers/auth/login.cpp");
  EXPECT_NE(c.find("Secure"), std::string::npos) << "Set-Cookie must contain Secure in production";
  EXPECT_NE(c.find("Max-Age"), std::string::npos) << "should set Max-Age/Expires";
}

TEST(P3_Security, CsrfDoubleSubmitShouldBeEnforced) {
  auto c = read_file("src/handlers/auth/login.cpp");
  EXPECT_EQ(c.find("session_csrf=\"test-csrf-token\""), std::string::npos) << "must not fallback to test-csrf-token in production";
  EXPECT_NE(c.find("403"), std::string::npos) << "should return 403 on missing csrf";
}

TEST(P3_Infra, NginxShouldUseHttp11ForUWS) {
  auto n = read_file("nginx/nginx.conf");
  EXPECT_NE(n.find("proxy_http_version 1.1"), std::string::npos);
}

TEST(P3_Build, FetchContentShouldPinHash) {
  auto cm = read_file("CMakeLists.txt");
  EXPECT_NE(cm.find("URL_HASH"), std::string::npos) << "FetchContent should pin URL_HASH for supply chain";
  EXPECT_NE(cm.find("GIT_SHALLOW"), std::string::npos);
}

TEST(P3_Config, SecretsNotInEnvFile) {
  auto git = read_file(".gitignore");
  EXPECT_NE(git.find(".env"), std::string::npos);
  EXPECT_NE(read_file(".env.example").size(), 0u) << ".env.example should exist";
  std::ifstream f(".env");
  if(f){
    EXPECT_NE(git.find(".env"), std::string::npos) << ".env must be gitignored";
  }
}
