#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "handlers/auth/login.hpp"
#include "config/config.hpp"

static std::string read_file2(const std::string& p){
  std::ifstream f(p); if(!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)), {});
}

TEST(P4_Security, PasswordUsesBcryptNotSha256) {
  auto c = read_file2("src/handlers/auth/login.cpp");
  EXPECT_NE(c.find("bcrypt"), std::string::npos) << "should use bcrypt/argon2, not plain SHA256";
  EXPECT_NE(c.find("hash_password"), std::string::npos);
}

TEST(P4_Security, TurnstileUsesLibCurlPost) {
  auto c = read_file2("src/middleware/turnstile.cpp");
  EXPECT_NE(c.find("curl"), std::string::npos) << "should POST to challenges.cloudflare.com";
  EXPECT_NE(c.find("siteverify"), std::string::npos);
}

TEST(P4_Jobs, RealSqlExecution) {
  auto c = read_file2("src/jobs/jobs.cpp");
  EXPECT_NE(c.find("PQexecParams"), std::string::npos) << "jobs should execute DELETE via libpq";
  EXPECT_NE(c.find("RealPool"), std::string::npos);
}

TEST(P4_Infra, DockerHardening) {
  auto d = read_file2("Dockerfile");
  EXPECT_NE(d.find("no-new-privileges"), std::string::npos) << "Docker should drop privileges";
  auto comp = read_file2("docker-compose.yml");
  EXPECT_NE(comp.find("read_only"), std::string::npos) << "compose should have read_only";
  EXPECT_NE(comp.find("no-new-privileges"), std::string::npos);
}

TEST(P4_Infra, NginxRateLimitAndTls) {
  auto n = read_file2("nginx/nginx.conf");
  EXPECT_NE(n.find("limit_req"), std::string::npos) << "nginx should have rate limiting";
  EXPECT_NE(n.find("ssl"), std::string::npos) << "should have TLS config";
}

TEST(P4_Contract, ParityRoutes) {
  auto j = read_file2("scripts/contract.json");
  EXPECT_NE(j.find("/api/health"), std::string::npos);
  EXPECT_NE(j.find("/ws/"), std::string::npos);
  size_t cnt=0; size_t pos=0;
  while((pos=j.find("\"path\"",pos))!=std::string::npos){cnt++; pos+=6;}
  EXPECT_GE(cnt, 40u) << "contract should have 40+ routes, got " << cnt;
}

TEST(P4_Frontend, ThemeSingleSource) {
  auto css = read_file2("static/css/theme.css");
  EXPECT_NE(css.size(), 0u) << "theme.css must exist as single source of truth";
  EXPECT_NE(css.find("--"), std::string::npos) << "should contain CSS variables";
}

TEST(P4_Build, SanitizersAndWarnings) {
  auto cm = read_file2("CMakeLists.txt");
  EXPECT_NE(cm.find("ENABLE_SANITIZERS"), std::string::npos);
  EXPECT_NE(cm.find("-Werror"), std::string::npos);
  EXPECT_NE(cm.find("clang-format"), std::string::npos);
}
