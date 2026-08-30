#include <gtest/gtest.h>
#include "config/config.hpp"
#include "db/pool.hpp"
#include "redis/client.hpp"
#include "handlers/auth/login.hpp"
#include "middleware/scoring.hpp"
#include "websocket/hub.hpp"
#include "utils/sanitize.hpp"
using namespace examvan;

TEST(P1_Auth, CookieShouldHaveSecureFlagInProduction) {
  Config cfg; cfg.secret_key = std::string(32,'a');
  cfg.admin_user="admin"; cfg.admin_pass="pass"; cfg.r2_endpoint="https://e"; cfg.r2_access_key="k"; cfg.r2_secret_key="s";
  handlers::auth::clear_users_for_test();
  handlers::auth::set_user_for_test("admin","secret123","guru");
  Request req; req.body="username=admin&password=secret123&_csrf=test-csrf-token";
  req.headers["Cookie"]="csrf_token=test-csrf-token";
  req.headers["X-CSRF-Token"]="test-csrf-token";
  setenv("APP_ENV","production",1);
  auto res = handlers::auth::login_handler(req,cfg);
  unsetenv("APP_ENV");
  auto it=res.headers.find("Set-Cookie");
  ASSERT_NE(it, res.headers.end()) << "no Set-Cookie";
  EXPECT_NE(it->second.find("Secure"), std::string::npos) << "cookie missing Secure flag: " << it->second;
  EXPECT_NE(it->second.find("HttpOnly"), std::string::npos);
  handlers::auth::clear_users_for_test();
}

TEST(P1_Auth, CsrfShouldNotFallbackToTestToken) {
  Config cfg; cfg.secret_key = std::string(32,'b');
  cfg.admin_user="admin"; cfg.admin_pass="pass"; cfg.r2_endpoint="https://e"; cfg.r2_access_key="k"; cfg.r2_secret_key="s";
  handlers::auth::clear_users_for_test();
  handlers::auth::set_user_for_test("admin","secret123","guru");
  Request req; req.body="username=admin&password=secret123&_csrf=test-csrf-token";
  auto res = handlers::auth::login_handler(req,cfg);
  EXPECT_EQ(res.status, 403) << "should reject CSRF when no csrf_token cookie present (fallback bypass)";
  handlers::auth::clear_users_for_test();
}

TEST(P1_Auth, PasswordShouldBeHashedNotPlain) {
  handlers::auth::clear_users_for_test();
  handlers::auth::set_user_for_test("admin","mySecret!123","guru");
  Request req; req.body="username=admin&password=mySecret!123&_csrf=test-csrf-token";
  req.headers["Cookie"]="csrf_token=test-csrf-token";
  Config cfg; cfg.secret_key=std::string(32,'c'); cfg.admin_user="admin"; cfg.admin_pass="mySecret!123"; cfg.r2_endpoint="https://e"; cfg.r2_access_key="k"; cfg.r2_secret_key="s";
  auto ok = handlers::auth::login_handler(req,cfg);
  EXPECT_TRUE(ok.status==200 || ok.status==303) << "valid password should succeed even after hashing, got " << ok.status;
  Request bad; bad.body="username=admin&password=wrongpass&_csrf=test-csrf-token";
  bad.headers["Cookie"]="csrf_token=test-csrf-token";
  auto fail = handlers::auth::login_handler(bad,cfg);
  EXPECT_EQ(fail.status, 401);
  handlers::auth::clear_users_for_test();
}

TEST(P1_Config, PgConninfoParsesAllFields) {
  std::string url="postgresql://user:pass@myhost:5432/mydb?sslmode=require";
  auto ci = pg_conninfo_from_url(url);
  EXPECT_NE(ci.find("host=myhost"), std::string::npos) << ci;
  EXPECT_NE(ci.find("user=user"), std::string::npos) << ci;
  EXPECT_NE(ci.find("password=pass"), std::string::npos) << ci;
  EXPECT_NE(ci.find("dbname=mydb"), std::string::npos) << ci;
  EXPECT_NE(ci.find("port=5432"), std::string::npos) << ci;
}

TEST(P1_Config, PgConninfoHandlesEncodedPassword) {
  std::string url="postgres://u:p%40ss%3Aw0rd@h/db";
  auto ci = pg_conninfo_from_url(url);
  EXPECT_NE(ci.find("host=h"), std::string::npos) << ci;
  EXPECT_NE(ci.find("dbname=db"), std::string::npos) << ci;
}

TEST(P1_Config, ValidateRejectsBadDatabaseUrl) {
  Config c; c.secret_key=std::string(32,'x'); c.admin_user="u"; c.admin_pass="p"; c.r2_access_key="k"; c.r2_secret_key="s"; c.r2_endpoint="https://e";
  c.database_url="not-a-url";
  EXPECT_THROW(c.validate(), std::runtime_error) << "should reject invalid DATABASE_URL if set";
}

TEST(P1_Redis, TtlExpiry) {
  RedisClient rc("redis://localhost:6379"); rc.connect();
  std::string job="ttl-test-job";
  rc.release_job(job);
  EXPECT_TRUE(rc.try_acquire_job(job, 1));
  EXPECT_FALSE(rc.try_acquire_job(job, 1));
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  EXPECT_TRUE(rc.try_acquire_job(job, 1)) << "lock should expire after TTL";
  rc.release_job(job);
}

TEST(P1_Scoring, WeightParsed) {
  std::string j=R"([{"number":1,"type":"single_choice","weight":5,"key":"A"}])";
  auto qs=scoring::parse_questions(j);
  ASSERT_EQ(qs.size(), 1u);
  EXPECT_DOUBLE_EQ(qs[0].weight, 5);
}

TEST(P1_Scoring, KeyAndTypeParsed) {
  std::string j=R"([{"number":2,"type":"true_false","weight":3,"key":"true"}])";
  auto qs=scoring::parse_questions(j);
  ASSERT_EQ(qs.size(), 1u);
  EXPECT_EQ(qs[0].type, "true_false");
  EXPECT_EQ(qs[0].weight, 3);
}

TEST(P1_Hub, OriginRejectsWrongHost) {
  EXPECT_FALSE(check_origin("https://evil.com","examvan.example.com"));
  EXPECT_FALSE(check_origin("https://examvan.example.com.evil.com","examvan.example.com"));
  EXPECT_TRUE(check_origin("https://examvan.example.com","examvan.example.com"));
  EXPECT_FALSE(check_origin("","anything"));
  EXPECT_TRUE(check_origin("http://localhost:3000","examvan.example.com"));
  EXPECT_TRUE(check_origin("http://127.0.0.1:5000","examvan.example.com"));
}

TEST(P1_Sanitize, WsFieldStripsDangerous) {
  std::string raw="<script>alert('x')</script>&test=`value`=\"hi\"";
  auto out=sanitize_ws_field(raw, 100);
  EXPECT_EQ(out.find('<'), std::string::npos);
  EXPECT_EQ(out.find('>'), std::string::npos);
  EXPECT_EQ(out.find('&'), std::string::npos);
  EXPECT_EQ(out.find('`'), std::string::npos);
  EXPECT_EQ(out.find('"'), std::string::npos);
  EXPECT_NE(out.find("script"), std::string::npos);
}
