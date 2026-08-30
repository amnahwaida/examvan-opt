#include <gtest/gtest.h>
#include "session/cookie.hpp"
#include "server/server.hpp"
#include "config/config.hpp"
#include "handlers/admin/users.hpp"
#include "middleware/turnstile.hpp"
#include <fstream>
#include <cstdlib>

using namespace examvan;

static std::string rf(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)), {});
}

TEST(R3_S02_Users, NoXRoleHeaderBypass){
  auto c=rf("src/handlers/admin/users.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_EQ(c.find("X-Role"), std::string::npos) << "create_user must NOT trust X-Role header - use session role";
  EXPECT_NE(c.find("verify_session"), std::string::npos) << "should verify session to get role";
}

TEST(R3_S02_Users, XRoleHeaderDoesNotGrantOperator){
  Request req;
  req.body="username=testop&password=12345678&role=operator";
  req.headers["X-Role"]="superadmin";
  auto res=handlers::admin::create_user(req);
  EXPECT_EQ(res.status, 403) << "without valid session, even X-Role superadmin must be rejected: " << res.body;
}

TEST(R3_S02_Users, ValidSessionSuperadminCanCreateOperator){
  std::string secret="0123456789abcdef0123456789ABCDEF";
  std::string payload=b64_encode("admin_id=1&username=superadmin&role=superadmin&is_super_admin=1");
  std::string cookie_val=encode_cookie_value(secret, payload);
  setenv("EXAMVAN_SECRET", secret.c_str(), 1);
  Request req;
  req.body="username=testop2&password=12345678&role=operator";
  req.headers["Cookie"]="examvan_session="+cookie_val;
  auto res=handlers::admin::create_user(req);
  EXPECT_EQ(res.status, 201) << "superadmin session should allow operator creation: " << res.body;
  unsetenv("EXAMVAN_SECRET");
}

TEST(R3_S07_Turnstile, NoTrueOn200WithoutSuccess){
  auto c=rf("src/middleware/turnstile.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_EQ(c.find("return code==200"), std::string::npos) << "must not return true on code==200 without success:true";
  EXPECT_NE(c.find("success"), std::string::npos);
}

TEST(R3_S07_Turnstile, EmptySecretAlwaysFalse){
  setenv("APP_ENV","production",1);
  unsetenv("TURNSTILE_BYPASS");
  EXPECT_FALSE(middleware::verify_turnstile("some-token","","1.2.3.4"));
  unsetenv("APP_ENV");
}

TEST(R3_S06_Server, BodyClearedOnAbort){
  auto c=rf("src/server/server.cpp");
  ASSERT_FALSE(c.empty());
  auto pos=c.find("onAborted");
  ASSERT_NE(pos, std::string::npos);
  std::string seg=c.substr(pos, 600);
  EXPECT_NE(seg.find("body.clear()"), std::string::npos) << "onAborted must clear thread_local body: " << seg;
}

TEST(R3_S06_Server, NoRawNewAppLeak){
  auto c=rf("src/server/server.cpp");
  EXPECT_EQ(c.find("g_app = new uWS::App"), std::string::npos) << "should not leak new App, use smart pointer";
}

TEST(R3_S08_Nginx, CSPInAllLocations){
  auto c=rf("nginx/nginx.conf");
  ASSERT_FALSE(c.empty());
  auto loc_root=c.find("location / {");
  ASSERT_NE(loc_root, std::string::npos);
  auto end=c.find("}", loc_root);
  std::string block=c.substr(loc_root, end-loc_root);
  EXPECT_NE(block.find("Content-Security-Policy"), std::string::npos) << "location / must have CSP header: " << block;
  auto loc_health=c.find("location /api/health");
  if(loc_health!=std::string::npos){
    auto end2=c.find("}", loc_health);
    std::string b2=c.substr(loc_health, end2-loc_health);
    EXPECT_NE(b2.find("Content-Security-Policy"), std::string::npos) << "location /api/health must have CSP: " << b2;
  }
  auto loc_login=c.find("location ~ ^/(login|admin/login)");
  if(loc_login!=std::string::npos){
    auto end3=c.find("}", loc_login);
    std::string b3=c.substr(loc_login, end3-loc_login);
    EXPECT_NE(b3.find("Content-Security-Policy"), std::string::npos) << "login location must have CSP: " << b3;
  }
}

TEST(R3_S03_Main, HubWiredToRedis){
  auto c=rf("src/main.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("redis_real::redis_set"), std::string::npos) << "Hub should wire redis_real::redis_set not noop";
  EXPECT_EQ(c.find("(void)k;(void)v"), std::string::npos) << "noop lambda still present";
}

TEST(R3_S03_Main, QueueWiredToScoring){
  auto c=rf("src/main.cpp");
  EXPECT_NE(c.find("scoring"), std::string::npos) << "Worker should use scoring scorer not nullptr";
  EXPECT_EQ(c.find("Worker w(&sq, nullptr)"), std::string::npos) << "Worker nullptr wiring still present";
}

TEST(R3_Health, HasSixKeys){
  Config cfg; cfg.version="2.7.2"; cfg.port=5000;
  std::string j=server::health_json(cfg);
  EXPECT_NE(j.find("\"status\""), std::string::npos);
  EXPECT_NE(j.find("\"version\""), std::string::npos);
  EXPECT_NE(j.find("\"uwebsockets\""), std::string::npos);
  int keys=0;
  for(auto k: std::vector<std::string>{"status","version","uwebsockets"}){
    if(j.find("\""+k+"\"")!=std::string::npos) keys++;
  }
  EXPECT_GE(keys, 3);
  // require at least 6 keys after fix
  size_t count=0;
  size_t p=0;
  while((p=j.find("\":",p))!=std::string::npos){ count++; p+=2; }
  EXPECT_GE(count, 6u) << "health_json should have >=6 keys: " << j;
}

TEST(R3_Jobs, NoHardcodedUrl){
  auto c=rf("src/jobs/jobs.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_EQ(c.find("redis://localhost:6379"), std::string::npos) << "jobs must not hardcode redis url";
  EXPECT_EQ(c.find("postgresql://examvan:pass@db:5432/examvan"), std::string::npos) << "jobs must not hardcode db url";
}
