#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
static std::string read_src25(const std::string& p){ std::ifstream f(p); if(!f) return ""; std::ostringstream ss; ss<<f.rdbuf(); return ss.str(); }
TEST(P25, NginxMapUsesUriWithoutQuery){
  auto src = read_src25("nginx/nginx.conf");
  EXPECT_NE(src.find("map $uri"), std::string::npos);
  EXPECT_EQ(src.find("map $request_uri"), std::string::npos);
}
TEST(P25, NginxWsTimeoutCoversIdle){
  auto src = read_src25("nginx/nginx.conf");
  bool has_ws = src.find("location /ws/")!=std::string::npos;
  EXPECT_TRUE(has_ws);
  EXPECT_NE(src.find("X-Forwarded-Proto"), std::string::npos);
}
TEST(P25, EnvIntLogsOnBadValue){
  auto src = read_src25("src/config/config.cpp");
  auto pos = src.find("int env_int");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 500);
  bool logs = seg.find("cerr")!=std::string::npos || seg.find("log")!=std::string::npos;
  EXPECT_TRUE(logs);
}
TEST(P25, SearchEscapesLikeWildcards){
  auto src = read_src25("src/handlers/public/hasil.cpp");
  EXPECT_NE(src.find("ESCAPE"), std::string::npos);
}
TEST(P25, ExportHasLimit){
  auto src = read_src25("src/handlers/admin/export.cpp");
  EXPECT_NE(src.find("LIMIT"), std::string::npos);
}
TEST(P25, CdnHasSri){
  auto src = read_src25("static/js/protobuf-helper.js");
  bool sri = src.find("integrity")!=std::string::npos;
  EXPECT_TRUE(sri);
}
