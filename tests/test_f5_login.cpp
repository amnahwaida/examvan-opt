#include <gtest/gtest.h>
#include "handlers/auth/login.hpp"
using namespace examvan::handlers::auth;

TEST(F5Login, PageHasCsrf) {
  examvan::Request req;
  auto res=login_page(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("_csrf"), std::string::npos);
  EXPECT_NE(res.headers["Set-Cookie"].find("csrf_token"), std::string::npos);
}

TEST(F5Login, CsrfMismatch403) {
  clear_users_for_test();
  set_user_for_test("guru","pass","guru");
  examvan::Config cfg; cfg.secret_key="test-secret-1234567890abcdef12345678";
  examvan::Request req; req.body="username=guru&password=pass&_csrf=wrong";
  req.headers["Cookie"]="csrf_token=test-csrf-token";
  req.headers["X-CSRF-Token"]="wrong";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,403);
  EXPECT_NE(res.body.find("CSRF"), std::string::npos);
}

TEST(F5Login, Success200SetsCookie) {
  clear_users_for_test();
  set_user_for_test("guru","pass123","guru");
  examvan::Config cfg; cfg.secret_key="test-secret-1234567890abcdef12345678";
  examvan::Request req; req.body="username=guru&password=pass123&_csrf=test-csrf-token";
  req.headers["Cookie"]="csrf_token=test-csrf-token";
  req.headers["X-CSRF-Token"]="test-csrf-token";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("success"), std::string::npos);
  EXPECT_NE(res.headers["Set-Cookie"].find("examvan_session"), std::string::npos);
  clear_users_for_test();
}

TEST(F5Login, InvalidCred401) {
  clear_users_for_test();
  set_user_for_test("guru","correct","guru");
  examvan::Config cfg; cfg.secret_key="s";
  examvan::Request req; req.body="username=guru&password=wrong&_csrf=test-csrf-token";
  req.headers["Cookie"]="csrf_token=test-csrf-token";
  req.headers["X-CSRF-Token"]="test-csrf-token";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,401);
}

TEST(F5Login, TurnstileBypass) {
  clear_users_for_test();
  set_user_for_test("guru","pass","guru");
  examvan::Config cfg; cfg.secret_key="s";
  examvan::Request req; req.body="username=guru&password=pass&_csrf=test-csrf-token&cf-turnstile-response=test-bypass-token";
  req.headers["Cookie"]="csrf_token=test-csrf-token";
  req.headers["X-CSRF-Token"]="test-csrf-token";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,200);
}
