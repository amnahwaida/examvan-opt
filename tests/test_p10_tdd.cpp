#include <gtest/gtest.h>
#include <fstream>
#include <string>
static std::string r10(const std::string& p){ std::ifstream f(p); if(!f) return ""; return std::string((std::istreambuf_iterator<char>(f)), {}); }

TEST(P10_Security, NoPlainPasswordInConfig) {
  auto c = r10("src/handlers/auth/login.cpp");
  EXPECT_EQ(c.find("stored==password"), std::string::npos) << "should use hashed comparison";
  EXPECT_NE(c.find("hash_password"), std::string::npos);
}

TEST(P10_Api, AdminApiGuardUsesSessionNotHeader) {
  auto r = r10("src/http/router_full.cpp");
  EXPECT_NE(r.find("is_authenticated"), std::string::npos);
  EXPECT_EQ(r.find("X-Role"), std::string::npos) << "should not rely on X-Role header";
}

TEST(P10_Queue, JsonEscapeUsesHelper) {
  auto q = r10("src/queue/submission_queue.cpp");
  EXPECT_NE(q.find("json_escape"), std::string::npos);
  EXPECT_NE(q.find("json_unescape"), std::string::npos);
}

TEST(P10_Docs, MigrasiStatusHasF0F8) {
  auto m = r10("MIGRASI_STATUS.md");
  EXPECT_NE(m.find("F0"), std::string::npos);
  EXPECT_NE(m.find("F8"), std::string::npos);
  EXPECT_NE(m.find("REHEARSAL"), std::string::npos);
}
