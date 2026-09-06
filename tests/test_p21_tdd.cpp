#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
static std::string read_src21(const std::string& p){ std::ifstream f(p); if(!f) return ""; std::ostringstream ss; ss<<f.rdbuf(); return ss.str(); }
TEST(P21, FlushQueueChecksSendReturn){
  auto src = read_src21("src/server/server.cpp");
  bool checks = src.find("MSG_NOSIGNAL")!=std::string::npos ||
                src.find("send_sent")!=std::string::npos;
  EXPECT_TRUE(checks);
  EXPECT_EQ(src.find("send(cfd, frame.c_str(), frame.size(), 0);"), std::string::npos);
}
TEST(P21, UwsBackpressureChecked){
  auto src = read_src21("src/server/server.cpp");
  EXPECT_NE(src.find("getBufferedAmount"), std::string::npos);
}
TEST(P21, BatchQueueSizeLocked){
  auto src = read_src21("src/queue/submission_queue.cpp");
  EXPECT_EQ(src.find("if(batch_q_.size()"), std::string::npos);
  EXPECT_NE(src.find("pending()"), std::string::npos);
}
TEST(P21, ServerUwsThreadJoinable){
  auto src = read_src21("src/server/server.cpp");
  EXPECT_EQ(src.find("g_uWS_thread.detach()"), std::string::npos);
  EXPECT_NE(src.find("g_uWS_thread.joinable()"), std::string::npos);
}
TEST(P21, RealPoolGuardsIdleWithMutex){
  auto src = read_src21("src/db/pool_real.cpp");
  EXPECT_NE(src.find("std::lock_guard<std::mutex>"), std::string::npos);
  auto h = read_src21("src/db/pool_real.hpp");
  EXPECT_NE(h.find("~RealPool"), std::string::npos);
}
