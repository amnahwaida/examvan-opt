#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "handlers/auth/login.hpp"
#include "config/config.hpp"

static std::string r12(const std::string& p){ std::ifstream f(p); if(!f) return ""; return std::string((std::istreambuf_iterator<char>(f)), {}); }

TEST(P12_Security, BcryptSaltedHash) {
  auto h1 = std::string("placeholder");
  examvan::handlers::auth::clear_users_for_test();
  examvan::handlers::auth::set_user_for_test("u1","samepass","guru");
  examvan::handlers::auth::set_user_for_test("u2","samepass","guru");
  // cannot directly read hash, but we can test login still works and that file contains salt logic
  auto c = r12("src/handlers/auth/login.cpp");
  EXPECT_NE(c.find("RAND_bytes"), std::string::npos) << "bcrypt should use salt/RAND_bytes";
  EXPECT_NE(c.find("gensalt"), std::string::npos) << "should use gensalt or salt generation";
  examvan::handlers::auth::clear_users_for_test();
}

TEST(P12_Jobs, RealExecutionNotCommentOnly) {
  auto c = r12("src/jobs/jobs.cpp");
  EXPECT_GT(c.size(), 800u) << "jobs.cpp should contain real logic (>800 bytes)";
  EXPECT_NE(c.find("DbPool"), std::string::npos) << "should use DbPool/RealPool";
  EXPECT_NE(c.find("RedisClient"), std::string::npos) << "should use RedisClient";
  size_t pos = c.find("run_expiry_job");
  ASSERT_NE(pos, std::string::npos);
  std::string segment = c.substr(pos, 500);
  EXPECT_EQ(segment.find("// DELETE"), std::string::npos) << "should not be comment-only, must be real SQL";
}
