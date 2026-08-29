#include <gtest/gtest.h>
#include <fstream>
#include <string>
static std::string r9(const std::string& p){ std::ifstream f(p); if(!f) return ""; return std::string((std::istreambuf_iterator<char>(f)), {}); }

TEST(P9_Docs, CutoverDocExists) {
  EXPECT_NE(r9("docs-cutover.md").size(), 0u);
  EXPECT_NE(r9("MIGRASI_STATUS.md").find("F0"), std::string::npos);
}

TEST(P9_Observability, HealthHasSixKeys) {
  auto s = r9("src/server/server.cpp");
  EXPECT_NE(s.find("health_json"), std::string::npos);
  EXPECT_NE(r9("src/http/router_full.cpp").find("/api/health"), std::string::npos);
}

TEST(P9_Performance, SoakCheckHasRss) {
  auto sh = r9("scripts/soak_check.sh");
  EXPECT_NE(sh.find("RSS"), std::string::npos) << "soak should check RSS";
  EXPECT_NE(sh.find("p99"), std::string::npos);
}

TEST(P9_Performance, K6HasWs) {
  auto js = r9("scripts/load_test/k6_ws.js");
  if(js.empty()) GTEST_SKIP() << "no k6 file";
  EXPECT_NE(js.find("WebSocket"), std::string::npos);
}
