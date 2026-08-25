#include <gtest/gtest.h>
#include "handlers/auth/logout.hpp"
using namespace examvan::handlers::auth;
TEST(F5Logout, GetRedirectsToLogin){ examvan::Request req; auto r=logout_page(req); EXPECT_EQ(r.status,302); EXPECT_EQ(r.headers["Location"],"/login"); }
TEST(F5Logout, PostRequiresCsrf){ examvan::Request req; req.body=""; req.headers["Cookie"]="csrf_token=test-csrf-token"; req.headers["X-CSRF-Token"]="wrong"; auto r=logout_handler(req); EXPECT_EQ(r.status,403); }
TEST(F5Logout, PostSuccessClearsCookie){ examvan::Request req; req.body="_csrf=test-csrf-token"; req.headers["Cookie"]="csrf_token=test-csrf-token"; req.headers["X-CSRF-Token"]="test-csrf-token"; auto r=logout_handler(req); EXPECT_EQ(r.status,200); EXPECT_NE(r.headers["Set-Cookie"].find("Max-Age=0"), std::string::npos); }
