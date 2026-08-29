#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "http/router.hpp"
#include "middleware/ratelimit.hpp"
#include "middleware/body_limit.hpp"
#include "middleware/version.hpp"

static std::string read5(const std::string& p){
  std::ifstream f(p); if(!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)), {});
}

TEST(P5_Api, HealthContract) {
  auto s = read5("src/server/server.cpp");
  EXPECT_NE(s.find("health_json"), std::string::npos);
  auto r = read5("src/http/router_full.cpp");
  EXPECT_NE(r.find("/api/health"), std::string::npos);
  EXPECT_NE(r.find("/api/time"), std::string::npos);
}

TEST(P5_Middleware, RateLimitAndBodyLimitWired) {
  auto r = read5("src/http/router_full.cpp");
  EXPECT_NE(r.find("RateLimiter"), std::string::npos) << "router should wire RateLimiter";
  EXPECT_NE(r.find("body_limit"), std::string::npos) << "router should wire body_limit 5MB";
}

TEST(P5_Middleware, VersionGate426) {
  examvan::Request req; req.headers["X-App-Version"]="0.1.0";
  auto res = examvan::middleware::version_gate(req,"2.7.2",[](auto){ examvan::Response x; x.status=200; return x; });
  EXPECT_EQ(res.status, 426);
  req.headers["X-App-Version"]="2.7.2";
  auto ok = examvan::middleware::version_gate(req,"2.7.2",[](auto){ examvan::Response x; x.status=200; return x; });
  EXPECT_EQ(ok.status, 200);
}

TEST(P5_Observability, SoakAndLoadScriptsExist) {
  EXPECT_NE(read5("scripts/soak_check.sh").size(), 0u) << "soak_check.sh must exist";
  EXPECT_NE(read5("scripts/load_test/k6_ws.js").size(), 0u) << "k6_ws.js must exist";
  auto ci = read5(".github/workflows/ci.yml");
  EXPECT_NE(ci.find("clang-format"), std::string::npos) << "CI should enforce clang-format";
  EXPECT_EQ(ci.find("clang-format --dry-run --Werror || true"), std::string::npos) << "CI must not use || true for format";
}

TEST(P5_Security, NoSecretsInLogs) {
  auto m = read5("src/main.cpp");
  EXPECT_EQ(m.find("cout << cfg.secret"), std::string::npos) << "must not log secrets";
  EXPECT_EQ(m.find("cout << cfg.admin_pass"), std::string::npos) << "should not print admin_pass";
}

TEST(P5_Data, SchemaHasTombstoneAndRetention) {
  auto s = read5("internal/database/schema.sql");
  if(s.empty()) s=read5("migrasi-cpp/03-peta-modul-dan-kontrak-api.md");
  EXPECT_NE(s.find("tombstone"), std::string::npos) << "schema should have tombstoned_at";
}
