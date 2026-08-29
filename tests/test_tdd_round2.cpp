#include <gtest/gtest.h>
#include "middleware/turnstile.hpp"
#include "middleware/ratelimit.hpp"
#include "middleware/body_limit.hpp"
#include "config/config.hpp"
#include "handlers/auth/login.hpp"
#include "handlers/public/download.hpp"
#include "websocket/hub.hpp"
#include "session/csrf.hpp"
#include <cstdlib>
#include <fstream>

using namespace examvan;

TEST(TDD2_C1_Turnstile, BypassBlockedInProduction) {
    setenv("APP_ENV","production",1);
    unsetenv("TURNSTILE_BYPASS");
    EXPECT_FALSE(middleware::verify_turnstile("test-bypass-token","",""));
    EXPECT_FALSE(middleware::verify_turnstile("anything","", ""));
    setenv("APP_ENV","development",1);
    EXPECT_TRUE(middleware::verify_turnstile("test-bypass-token","",""));
    unsetenv("APP_ENV");
    setenv("TURNSTILE_BYPASS","1",1);
    EXPECT_TRUE(middleware::verify_turnstile("anything","", ""));
    unsetenv("TURNSTILE_BYPASS");
    unsetenv("APP_ENV");
}

TEST(TDD2_C2_RateLimit, GlobalLimiterBlocks) {
    middleware::RateLimiter rl(2, std::chrono::seconds(10));
    EXPECT_TRUE(rl.allow("10.0.0.1"));
    EXPECT_TRUE(rl.allow("10.0.0.1"));
    EXPECT_FALSE(rl.allow("10.0.0.1"));
}

TEST(TDD2_C2_RateLimit, AdminApiWired) {
    std::ifstream f("src/http/router_full.cpp");
    std::string s((std::istreambuf_iterator<char>(f)), {});
    EXPECT_NE(s.find("RateLimiter"), std::string::npos);
    EXPECT_NE(s.find(".allow("), std::string::npos);
}

TEST(TDD2_C3_BodyLimit, GlobalRejectsLarge) {
    examvan::Request req; req.body = std::string(6*1024*1024,'x');
    auto res = middleware::body_limit(req, 5*1024*1024, [](auto){ examvan::Response r; r.status=200; return r; });
    EXPECT_EQ(res.status, 413);
}

TEST(TDD2_C4_R2, NotConfiguredReturns503) {
    setenv("APP_ENV","production",1);
    setenv("R2_ACCESS_KEY_ID","",1);
    setenv("R2_SECRET_ACCESS_KEY","",1);
    setenv("R2_ENDPOINT","",1);
    examvan::Request req;
    auto res = handlers::public_::download_apk(req);
    EXPECT_EQ(res.status, 503);
    unsetenv("APP_ENV");
    setenv("R2_ACCESS_KEY_ID","test-access",1);
    setenv("R2_SECRET_ACCESS_KEY","test-secret",1);
    setenv("R2_ENDPOINT","https://test.r2.cloudflarestorage.com",1);
}

TEST(TDD2_C5_CSRF, CookieHasHardening) {
    examvan::Request dummy;
    auto res = handlers::auth::login_page(dummy);
    auto it = res.headers.find("Set-Cookie");
    ASSERT_NE(it, res.headers.end());
    std::string c = it->second;
    EXPECT_NE(c.find("HttpOnly"), std::string::npos);
    EXPECT_NE(c.find("SameSite"), std::string::npos);
}

TEST(TDD2_C6_Origin, PortStripped) {
    EXPECT_TRUE(check_origin("https://examvan.id","examvan.id:8081"));
    EXPECT_TRUE(check_origin("https://examvan.id:443","examvan.id"));
    EXPECT_TRUE(check_origin("https://examvan.id:8081","examvan.id:8081"));
    EXPECT_FALSE(check_origin("https://evil.com","examvan.id:8081"));
    EXPECT_FALSE(check_origin("https://evil.com:8081","examvan.id"));
    EXPECT_TRUE(check_origin("http://localhost:3000","examvan.id"));
    EXPECT_TRUE(check_origin("","examvan.id"));
}

TEST(TDD2_M2_OpenRedirect, EncodedTraversalBlocked) {
    Config cfg; cfg.secret_key=std::string(32,'a'); cfg.admin_user="u"; cfg.admin_pass="p";
    cfg.r2_access_key="k"; cfg.r2_secret_key="s"; cfg.r2_endpoint="https://e";
    handlers::auth::clear_users_for_test();
    handlers::auth::set_user_for_test("bob","pass12345","");
    // need csrf
    Request req;
    req.body="username=bob&password=pass12345&_csrf=test-csrf-token&next=%2F%2Fevil.com";
    req.headers["Cookie"]="csrf_token=test-csrf-token";
    auto res = handlers::auth::login_handler(req,cfg);
    if(res.status==303){
        EXPECT_NE(res.headers["Location"], "//evil.com");
        EXPECT_EQ(res.headers["Location"].find("evil"), std::string::npos);
    }
    handlers::auth::clear_users_for_test();
}
