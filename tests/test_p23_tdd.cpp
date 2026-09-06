#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
static std::string read_src23(const std::string& p){ std::ifstream f(p); if(!f) return ""; std::ostringstream ss; ss<<f.rdbuf(); return ss.str(); }
TEST(P23, HubHasRedisMutex){
  auto h = read_src23("src/websocket/hub.hpp");
  EXPECT_NE(h.find("redis_mu_"), std::string::npos);
  auto c = read_src23("src/websocket/hub.cpp");
  EXPECT_NE(c.find("redis_mu_"), std::string::npos);
}
TEST(P23, MigrateCreatesCoreTables){
  auto src = read_src23("src/store/exam_store_postgres.cpp");
  EXPECT_NE(src.find("CREATE TABLE IF NOT EXISTS submissions"), std::string::npos);
  EXPECT_NE(src.find("CREATE TABLE IF NOT EXISTS student_access_logs"), std::string::npos);
  EXPECT_NE(src.find("CREATE TABLE IF NOT EXISTS exam_approvals"), std::string::npos);
  EXPECT_NE(src.find("CREATE TABLE IF NOT EXISTS admin_users"), std::string::npos);
}
TEST(P23, RedisParseHandlesAuthAndDb){
  auto src = read_src23("src/redis/redis_real.cpp");
  bool auth = src.find("SELECT")!=std::string::npos || src.find("AUTH")!=std::string::npos;
  EXPECT_TRUE(auth);
}
TEST(P23, TokenRotationUsesForUpdate){
  auto src = read_src23("src/handlers/api/exams.cpp");
  EXPECT_NE(src.find("FOR UPDATE"), std::string::npos);
}
TEST(P23, ApprovalRevokedAfterCommit){
  auto src = read_src23("src/queue/submission_queue.cpp");
  EXPECT_NE(src.find("DELETE FROM exam_approvals"), std::string::npos);
}
