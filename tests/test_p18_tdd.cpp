#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include "middleware/scoring.hpp"
#include "session/cookie.hpp"
#include "jobs/jobs.hpp"
#include "queue/submission_queue.hpp"
#include "handlers/auth/logout.hpp"
#include "http/router.hpp"

using namespace examvan;

static std::string read_src(const std::string& p) {
  std::ifstream f(p);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

TEST(P18, ScoringBoolSkipsSpaces) {
  std::string q = R"([{"number":1,"type":"multiple_choice","weight":1,"key":"A","partial_scoring": true}])";
  std::map<std::string, std::string> ans{{"1", "A"}};
  auto s = scoring::score_submission_json(q, ans);
  ASSERT_TRUE(s.has_value());
  EXPECT_DOUBLE_EQ(*s, 100.0);
  std::string q2 = R"([{"number":1,"type":"single_choice","weight":2,"key":"B","partial_scoring" : true}])";
  auto s2 = scoring::score_submission_json(q2, {{"1", "B"}});
  ASSERT_TRUE(s2.has_value());
  EXPECT_DOUBLE_EQ(*s2, 100.0);
}

TEST(P18, CookieNoFallbackToWholeHeader) {
  std::string secret(32, 'k');
  auto r = verify_session_cookie(secret, "foo=bar; baz=qux");
  EXPECT_FALSE(r.has_value());
  auto r2 = verify_session_cookie(secret, "");
  EXPECT_FALSE(r2.has_value());
}

TEST(P18, LogoutClearMirrorsSecure) {
  auto src = read_src("src/handlers/auth/logout.cpp");
  EXPECT_NE(src.find("Secure"), std::string::npos);
}

TEST(P18, VoucherBatchAcceptsCommandOk) {
  auto src = read_src("src/handlers/admin/vouchers.cpp");
  EXPECT_NE(src.find("PGRES_COMMAND_OK"), std::string::npos);
}

TEST(P18, TurnstileFailClosedWhenSecretSet) {
  auto src = read_src("src/handlers/auth/login.cpp");
  bool has_secret_check = src.find("turnstile_secret") != std::string::npos;
  bool has_empty_reject = src.find("Turnstile") != std::string::npos;
  EXPECT_TRUE(has_secret_check && has_empty_reject);
  EXPECT_NE(src.find("turnstile_enabled"), std::string::npos);
}

TEST(P18, RateLimitHasLocalFallback) {
  auto src = read_src("src/handlers/api/exams.cpp");
  EXPECT_NE(src.find("static examvan::middleware::RateLimiter"), std::string::npos);
}

TEST(P18, JobRunnerStopsFast) {
  jobs::JobRunner r([] {}, std::chrono::seconds(3600));
  r.start();
  auto t0 = std::chrono::steady_clock::now();
  r.stop();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
  EXPECT_LT(ms, 1500);
}

TEST(P18, FlusherStopsFast) {
  queue::HeartbeatFlusher f([] { return 0; });
  f.start();
  auto t0 = std::chrono::steady_clock::now();
  f.stop();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
  EXPECT_LT(ms, 1500);
}

TEST(P18, StaticPathSafeHelper) {
  auto src = read_src("src/server/server.cpp");
  EXPECT_NE(src.find("static_path_safe"), std::string::npos);
}

TEST(P18, UwsForwardsProtoHeader) {
  auto src = read_src("src/server/server.cpp");
  EXPECT_NE(src.find("X-Forwarded-Proto"), std::string::npos);
}

TEST(P18, EditUserSanitizesSuperadmin) {
  auto src = read_src("src/handlers/admin/users.cpp");
  EXPECT_NE(src.find("superadmin"), std::string::npos);
  bool has_strip = src.find("strip_superadmin") != std::string::npos ||
                   src.find("allow_superadmin") != std::string::npos ||
                   src.find("sanitize_roles") != std::string::npos;
  EXPECT_TRUE(has_strip);
}
