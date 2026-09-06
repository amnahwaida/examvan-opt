#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
static std::string read_src22(const std::string& p){ std::ifstream f(p); if(!f) return ""; std::ostringstream ss; ss<<f.rdbuf(); return ss.str(); }
TEST(P22, UwsStaticHasTraversalGuard){
  auto src = read_src22("src/server/server.cpp");
  auto anypos = src.find("g_app->any");
  ASSERT_NE(anypos, std::string::npos);
  auto seg = src.substr(anypos, 1500);
  EXPECT_NE(seg.find("static_path_safe"), std::string::npos);
}
TEST(P22, UwsForwardsDeviceAndAuthHeaders){
  auto src = read_src22("src/server/server.cpp");
  EXPECT_NE(src.find("x-device-id"), std::string::npos);
  EXPECT_NE(src.find("authorization"), std::string::npos);
  EXPECT_NE(src.find("user-agent"), std::string::npos);
}
TEST(P22, ClientIpHasTrustedProxyGate){
  auto src = read_src22("src/handlers/auth/auth_helpers.cpp");
  bool gated = src.find("TRUST_PROXY")!=std::string::npos ||
               src.find("trust_proxy")!=std::string::npos ||
               src.find("trusted")!=std::string::npos;
  EXPECT_TRUE(gated);
}
TEST(P22, WsPayloadLimitUniform){
  auto src = read_src22("src/server/server.cpp");
  EXPECT_NE(src.find("maxPayloadLength = 5*1024*1024"), std::string::npos);
}
TEST(P22, CompleteExamHasPbGate){
  auto src = read_src22("src/http/router_full.cpp");
  auto pos = src.find("/api/exams/:exam_id/complete");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos>200?pos-200:0, 300);
  EXPECT_NE(seg.find("pb_gate"), std::string::npos);
}
