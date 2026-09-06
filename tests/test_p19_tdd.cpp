#include <gtest/gtest.h>
#include "session/cookie.hpp"
#include "handlers/auth/login.hpp"
#include "handlers/auth/logout.hpp"
#include <fstream>
#include <sstream>
using namespace examvan;
static std::string read_src19(const std::string& p){ std::ifstream f(p); if(!f) return ""; std::ostringstream ss; ss<<f.rdbuf(); return ss.str(); }
TEST(P19, SessionPayloadHasExpiry){
  std::string p = handlers::auth::build_login_session_payload(1,"admin","[\"guru\"]");
  std::string raw = b64_decode(p);
  EXPECT_NE(raw.find("exp="), std::string::npos);
}
TEST(P19, ExpiredSessionRejected){
  std::string secret(32,'s');
  std::string raw = "admin_id=1&username=a&role=guru&exp=1";
  std::string c = encode_cookie_value(secret, b64_encode(raw));
  auto r = verify_session_cookie(secret, std::string("examvan_session=")+c);
  EXPECT_FALSE(r.has_value());
}
TEST(P19, FreshSessionAccepted){
  std::string secret(32,'s');
  std::string p = handlers::auth::build_login_session_payload(7,"bob","[\"guru\"]");
  std::string c = encode_cookie_value(secret, p);
  auto r = verify_session_cookie(secret, std::string("examvan_session=")+c);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->admin_id, 7);
}
TEST(P19, LogoutPageClearsCookie){
  auto src = read_src19("src/handlers/auth/logout.cpp");
  auto pos = src.find("logout_page");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 800);
  EXPECT_NE(seg.find("Max-Age=0"), std::string::npos);
}
