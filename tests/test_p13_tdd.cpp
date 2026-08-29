#include <gtest/gtest.h>
#include <fstream>
#include <string>
static std::string r13(const std::string& p){ std::ifstream f(p); if(!f) return ""; return std::string((std::istreambuf_iterator<char>(f)), {}); }

TEST(P13_Security, BcryptRealFormat) {
  auto c = r13("src/handlers/auth/login.cpp");
  EXPECT_NE(c.find("$2b$"), std::string::npos) << "should use real bcrypt $2b$ format, not SHA256";
  EXPECT_NE(c.find("crypt"), std::string::npos);
}

TEST(P13_Jobs, TransactionalDelete) {
  auto j = r13("src/jobs/jobs.cpp");
  EXPECT_NE(j.find("BEGIN"), std::string::npos) << "DELETE should be transactional";
  EXPECT_NE(j.find("COMMIT"), std::string::npos);
}

TEST(P13_Repo, InternalNotInBuild) {
  auto cm = r13("CMakeLists.txt");
  EXPECT_EQ(cm.find("internal"), std::string::npos) << "CMake should not reference internal/ Go legacy";
  auto gi = r13(".gitignore");
  EXPECT_NE(gi.find("internal"), std::string::npos);
}
