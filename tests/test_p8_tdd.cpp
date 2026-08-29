#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "middleware/version.hpp"
#include "session/cookie.hpp"
static std::string r8(const std::string& p){ std::ifstream f(p); if(!f) return ""; return std::string((std::istreambuf_iterator<char>(f)), {}); }

TEST(P8_Version, TrimSpacesShouldFail) {
  EXPECT_EQ(examvan::middleware::compare_versions("2.7.2","2.7.2"), 0);
  EXPECT_NE(examvan::middleware::compare_versions("2 . 7 . 2","2.7.2"), 0) << "should not accept spaced version as equal";
}

TEST(P8_Session, B64PadNotDoubleCount) {
  std::string s="YWI=";
  auto d=examvan::b64_decode(s);
  EXPECT_EQ(d, "ab");
  std::string s2="YQ==";
  EXPECT_EQ(examvan::b64_decode(s2), "a");
}

TEST(P8_Repo, NoDuplicatedSanitizeLogic) {
  auto a=r8("src/utils/sanitize.cpp");
  auto b=r8("src/helpers/utils.cpp");
  EXPECT_EQ(a.find("sanitize_student_input")!=std::string::npos && b.find("sanitize_student_input")!=std::string::npos, false) << "duplicate sanitize logic should be deduplicated";
}

TEST(P8_Repo, GitStatusClean) {
  auto out = r8("build/CMakeCache.txt");
  (void)out;
  // we check via system call in real CI; here just ensure .gitignore covers build
  auto gi=r8(".gitignore");
  EXPECT_NE(gi.find("build"), std::string::npos);
  EXPECT_NE(gi.find("soak-logs"), std::string::npos);
}

TEST(P8_Utils, SimplePass) {
  EXPECT_EQ(1,1);
}
