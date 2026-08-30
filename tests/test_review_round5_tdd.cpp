#include <gtest/gtest.h>
#include <fstream>
#include <string>
static std::string rf(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)), {});
}
TEST(R5_Queue, BatchUsesRealPool){
  auto c=rf("src/queue/submission_queue.cpp");
  ASSERT_FALSE(c.empty());
  auto pos=c.find("void Worker::run_batch");
  ASSERT_NE(pos, std::string::npos);
  std::string seg=c.substr(pos, 1200);
  EXPECT_NE(seg.find("RealPool"), std::string::npos) << "run_batch should use RealPool for batch insert: " << seg;
  EXPECT_NE(seg.find("exec_params"), std::string::npos);
}
TEST(R5_Cors, WiredInRouter){
  auto c=rf("src/http/router_full.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("cors"), std::string::npos) << "router_full should wire CORS (is_origin_allowed/cors_wrap): " << c.substr(0,500);
}
TEST(R5_Cache, CcacheEnabled){
  auto c=rf("CMakeLists.txt");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("ccache"), std::string::npos);
  bool has_find = c.find("find_program")!=std::string::npos && c.find("CCACHE")!=std::string::npos;
  bool has_launcher = c.find("COMPILER_LAUNCHER")!=std::string::npos;
  EXPECT_TRUE(has_find || has_launcher) << "CMake should have find_program ccache or COMPILER_LAUNCHER: " << c.substr(0,800);
}
TEST(R5_Users, NoDirectGetenvSecret){
  auto c=rf("src/handlers/admin/users.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_EQ(c.find("getenv(\"EXAMVAN_SECRET\")"), std::string::npos) << "users.cpp should not directly getenv secret, use Config or param";
  EXPECT_EQ(c.find("getenv(\"EXAMVAN_SECRET_PREV\")"), std::string::npos);
}
TEST(R5_CI, HasConcurrency){
  auto c=rf(".github/workflows/ci.yml");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("concurrency"), std::string::npos) << "ci.yml should have concurrency to save quota: " << c;
}
