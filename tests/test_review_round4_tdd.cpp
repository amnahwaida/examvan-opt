#include <gtest/gtest.h>
#include <fstream>
#include <string>

static std::string rf(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)), {});
}

TEST(R4_CI, LabelNot75){
  auto c=rf(".github/workflows/ci.yml");
  ASSERT_FALSE(c.empty());
  EXPECT_EQ(c.find("Unit tests (75)"), std::string::npos) << "CI label still 75, should be updated to actual count";
  EXPECT_NE(c.find("Unit tests (248)"), std::string::npos) << "CI should label Unit tests (248) matching current suite";
}

TEST(R4_Jobs, ApprovalCleanupUsesRealPool){
  auto c=rf("src/jobs/jobs.cpp");
  ASSERT_FALSE(c.empty());
  auto pos=c.find("run_approval_cleanup");
  ASSERT_NE(pos, std::string::npos);
  std::string seg=c.substr(pos, 800);
  EXPECT_NE(seg.find("RealPool"), std::string::npos) << "approval_cleanup should use RealPool exec_params like expiry: " << seg;
  EXPECT_NE(seg.find("exec_params"), std::string::npos);
}

TEST(R4_Jobs, AccessLogRetentionUsesRealPool){
  auto c=rf("src/jobs/jobs.cpp");
  auto pos=c.find("run_access_log_retention");
  ASSERT_NE(pos, std::string::npos);
  std::string seg=c.substr(pos, 800);
  EXPECT_NE(seg.find("RealPool"), std::string::npos) << "retention should use RealPool: " << seg;
  EXPECT_NE(seg.find("exec_params"), std::string::npos);
}

TEST(R4_Server, GracefulStopJoinsThread){
  auto c=rf("src/server/server.cpp");
  ASSERT_FALSE(c.empty());
  auto pos=c.find("void Server::stop()");
  ASSERT_NE(pos, std::string::npos);
  std::string seg=c.substr(pos, 600);
  EXPECT_NE(seg.find("join"), std::string::npos) << "stop() should join g_uWS_thread for graceful shutdown: " << seg;
}

TEST(R4_Health, UptimeNotHardcodedZero){
  auto c=rf("src/server/server.cpp");
  auto pos=c.find("health_json");
  ASSERT_NE(pos, std::string::npos);
  std::string seg=c.substr(pos, 600);
  EXPECT_NE(seg.find("steady_clock"), std::string::npos) << "health_json should compute real uptime via steady_clock not hardcode 0: " << seg;
  EXPECT_EQ(seg.find("\"uptime\":0}"), std::string::npos) << "should not hardcode uptime 0 at end";
}

TEST(R4_Queue, BatchHandlesNotJustPop){
  auto c=rf("src/queue/submission_queue.cpp");
  auto pos=c.find("void Worker::run_batch");
  ASSERT_NE(pos, std::string::npos);
  std::string seg=c.substr(pos, 900);
  bool has_db = seg.find("batch")!=std::string::npos;
  EXPECT_TRUE(has_db) << "run_batch should handle DB batch insert not just pop: " << seg;
  EXPECT_NE(seg.find("pop"), std::string::npos);
}
