#include <gtest/gtest.h>
#include "session/cookie.hpp"
#include "websocket/hub.hpp"
#include "helpers/utils.hpp"
#include "handlers/r2/r2.hpp"
#include "middleware/scoring.hpp"
#include "http/router.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
using namespace examvan;

static std::string read_file(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)), {});
}

TEST(Review_Nginx, RateLimitScopedToLogin){
  auto c=read_file("nginx/nginx.conf");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("limit_req_zone"), std::string::npos);
  EXPECT_EQ(c.find("limit_req zone=login burst=20 nodelay;"), std::string::npos) << "global limit_req in server block should be removed or moved";
  (void)(c.find("location ~ ^/(login|admin/login)")!=std::string::npos);
  // At least ensure not applied to location / with burst 20 globally
  auto server_pos=c.find("server {");
  auto loc_root=c.find("location / {", server_pos);
  if(loc_root!=std::string::npos){
    auto next_brace=c.find("}", loc_root);
    std::string loc_block=c.substr(loc_root, next_brace-loc_root);
    EXPECT_EQ(loc_block.find("limit_req"), std::string::npos) << "limit_req must NOT be in generic location / : " << loc_block;
  }
}

TEST(Review_Nginx, SecurityHeaders){
  auto c=read_file("nginx/nginx.conf");
  EXPECT_NE(c.find("Strict-Transport-Security"), std::string::npos) << "missing HSTS";
  EXPECT_NE(c.find("Content-Security-Policy"), std::string::npos) << "missing CSP";
  EXPECT_NE(c.find("client_max_body_size"), std::string::npos) << "missing client_max_body_size";
  EXPECT_EQ(c.find("proxy_read_timeout 3600s"), std::string::npos) << "3600s timeout excessive, should be 60s or less";
}

TEST(Review_Dockerfile, RuntimeSlim){
  auto c=read_file("Dockerfile");
  EXPECT_NE(c.find("debian:bookworm-slim"), std::string::npos) << "runtime should be debian slim, not gcc";
  EXPECT_EQ(c.find("FROM gcc:13-bookworm AS runtime"), std::string::npos) << "runtime must not be gcc";
  EXPECT_EQ(c.find("COPY .env.example"), std::string::npos) << "should not copy .env.example into image";
}

TEST(Review_Hub, BroadcastNoDeadlock){
  auto c=read_file("src/websocket/hub.cpp");
  // Must copy clients outside lock before try_send
  // Find broadcast_to_room and check pattern: should not hold mu_ while calling try_send directly inside lock without copy
  auto pos=c.find("broadcast_to_room");
  ASSERT_NE(pos, std::string::npos);
  std::string seg=c.substr(pos, 800);
  // Should contain vector copy or snapshot before calling try_send, or unlock before loop
  bool has_copy = seg.find("std::vector")!=std::string::npos && seg.find("try_send")!=std::string::npos;
  // Also check that mu_ is not held across try_send: we expect unlock before try_send or copy
  EXPECT_TRUE(has_copy) << "broadcast should copy clients under lock then release before try_send to avoid deadlock: " << seg;
}

TEST(Review_Hub, CheckOriginHandlesIPv6AndEmpty){
  EXPECT_TRUE(check_origin("http://[::1]:3000","[::1]:5000"));
  EXPECT_TRUE(check_origin("http://[::1]:8080","[::1]"));
  EXPECT_FALSE(check_origin("https://evil.com","[::1]:5000"));
  // empty origin for websocket: should allow only if we consider non-browser, but current review says empty should not auto-true for privileged? Keep true for now but test strip_port robust
  auto c=read_file("src/websocket/hub.cpp");
  EXPECT_NE(c.find("strip_port"), std::string::npos);
  // strip_port should handle bracket
  EXPECT_NE(c.find("["), std::string::npos) << "strip_port should handle IPv6 bracket";
}

TEST(Review_Session, ParseKvUrlDecode){
  std::string secret="test_secret_12345678901234567890abcd";
  std::string raw="admin_id=1&username=a%26b&role=guru&instansi=x%3Dy";
  // b64 encode raw then sign
  std::string payload=b64_encode(raw);
  std::string cookie_val=encode_cookie_value(secret, payload);
  auto sess=verify_session_cookie(secret, "examvan_session="+cookie_val);
  ASSERT_TRUE(sess.has_value());
  EXPECT_EQ(sess->fields["username"], "a&b") << "parse_kv should url_decode value: got " << sess->fields["username"];
  EXPECT_EQ(sess->fields["instansi"], "x=y") << "parse_kv should url_decode : got " << sess->fields["instansi"];
}

TEST(Review_Session, ExtractCookieExact){
  std::string hdr="foo=1; examvan_session=correct_val; examvan_session_hack=wrong; other=2";
  EXPECT_EQ(extract_cookie(hdr,"examvan_session"), "correct_val");
  hdr="examvan_session=first; examvan_session=second";
  EXPECT_EQ(extract_cookie(hdr,"examvan_session"), "first");
  hdr="  examvan_session = spaced_val ; other=1";
  EXPECT_EQ(extract_cookie(hdr,"examvan_session"), "spaced_val");
}

TEST(Review_Scoring, NestedBraces){
  std::string j=R"([{"number":1,"type":"single_choice","weight":1,"label":"a {b} c","key":"A"}, {"number":2,"type":"single_choice","weight":1,"key":"B"}])";
  auto qs=scoring::parse_questions(j);
  ASSERT_EQ(qs.size(), 2u) << "should parse 2 questions even though label contains braces";
  EXPECT_EQ(qs[0].number, 1);
  EXPECT_EQ(qs[1].number, 2);
  std::string j2=R"([{"number":1,"key":"A, B","type":"multiple_choice","weight":2}])";
  auto qs2=scoring::parse_questions(j2);
  ASSERT_EQ(qs2.size(), 1u);
  EXPECT_EQ(qs2[0].key, "A, B");
}

TEST(Review_R2, RegionNotAuto){
  r2::R2Config cfg{"AK","SK","https://abc.r2.cloudflarestorage.com","mybucket"};
  auto url=r2::presign_url(cfg,"exams/1/file.pdf",3600);
  ASSERT_FALSE(url.empty());
  EXPECT_EQ(url.find("/auto/"), std::string::npos) << "should not use auto region: " << url;
  EXPECT_NE(url.find("%2F"), std::string::npos) << "credential must be encoded";
  // check key encoding: space should be %20
  r2::R2Config cfg2{"AK","SK","https://abc.r2.cloudflarestorage.com","mybucket"};
  auto url2=r2::presign_url(cfg2,"exams/1/my file.pdf",3600);
  EXPECT_NE(url2.find("%20"), std::string::npos) << "key should be uri-encoded: " << url2;
  EXPECT_EQ(url2.find("my file.pdf"), std::string::npos) << "raw space must not appear";
}

TEST(Review_Router, UrlDecodeParam){
  Router r;
  r.add("GET","/hasil/:token", [](const Request& req){ Response res; res.json(200, req.params.at("token")); return res; });
  Request req; req.method="GET"; req.path="/hasil/hello%20world";
  // router should url_decode param? Current does not, test expects decode
  auto res=r.dispatch(req);
  // after fix, token should be "hello world"
  EXPECT_NE(res.body.find("hello"), std::string::npos);
  // specifically check param decoding if implemented
  // we allow either encoded or decoded but prefer decoded
  bool decoded = res.body.find("hello world")!=std::string::npos;
  bool encoded = res.body.find("hello%20world")!=std::string::npos;
  EXPECT_TRUE(decoded || encoded) << "router param should be present: " << res.body;
  if(!decoded && encoded){
    // will fail after we require decode
    EXPECT_TRUE(decoded) << "router should url_decode param token, got encoded: " << res.body;
  }
}

TEST(Review_Server, StaticTraversalEncoded){
  auto c=read_file("src/server/server.cpp");
  // try_serve_static should block encoded traversal
  EXPECT_NE(c.find("url_decode"), std::string::npos) << "static handler should url_decode path before checking";
  EXPECT_NE(c.find("%2e"), std::string::npos) << "should check encoded dots";
  bool handles_percent = c.find("%2e")!=std::string::npos || c.find("percent")!=std::string::npos || c.find("decode")!=std::string::npos;
  EXPECT_TRUE(handles_percent);
}

TEST(Review_Login, NoRandFallback){
  auto c=read_file("src/handlers/auth/login.cpp");
  EXPECT_EQ(c.find("rand()"), std::string::npos) << "gensalt must not use rand() fallback";
  EXPECT_NE(c.find("RAND_bytes"), std::string::npos);
}

TEST(Review_Turnstile, UrlEncodeFields){
  auto c=read_file("src/middleware/turnstile.cpp");
  EXPECT_NE(c.find("curl_easy_escape"), std::string::npos) << "should urlencode fields";
  if(c.find("TURNSTILE_BYPASS")!=std::string::npos){
    // should have is_prod check before bypass
    auto pos=c.find("TURNSTILE_BYPASS");
    std::string seg=c.substr(pos>200?pos-200:0, 400);
    EXPECT_NE(seg.find("is_prod"), std::string::npos) << "BYPASS must be guarded by production check: " << seg;
  }
  // should parse JSON success not just code 200
  EXPECT_NE(c.find("success"), std::string::npos) << "should verify JSON success field";
}

TEST(Review_CSRF, CookieNotHttpOnly){
  auto c=read_file("src/handlers/auth/login.cpp");
  auto pos=c.find("csrf_token=");
  ASSERT_NE(pos, std::string::npos);
  std::string seg=c.substr(pos, 200);
  // For double-submit, csrf cookie must be readable by JS -> no HttpOnly
  EXPECT_EQ(seg.find("HttpOnly"), std::string::npos) << "csrf_token cookie must NOT be HttpOnly for double-submit: " << seg;
}

TEST(Review_Sanitize, HtmlEscapeUsedInDashboard){
  auto c=read_file("src/handlers/admin/dashboard.cpp");
  EXPECT_NE(c.find("html_escape"), std::string::npos) << "admin dashboard should use html_escape for SSR output";
}
