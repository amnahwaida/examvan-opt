#include <gtest/gtest.h>
#include "session/cookie.hpp"
using namespace examvan;

TEST(Session, B64RoundTrip) {
  std::string s="hello world";
  EXPECT_EQ(b64_decode(b64_encode(s)), s);
}

TEST(Session, HmacDeterministic) {
  auto a=hmac_sha256_b64("key","data");
  auto b=hmac_sha256_b64("key","data");
  EXPECT_EQ(a,b);
  EXPECT_NE(a, hmac_sha256_b64("key2","data"));
}

TEST(Session, EncodeDecode) {
  std::string secret="examvan-secret-0123456789abcdef";
  std::string payload = b64_encode("admin_id=1&username=superadmin");
  auto cookie = encode_cookie_value(secret, payload);
  auto decoded = decode_cookie_value(secret, cookie);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, "admin_id=1&username=superadmin");
}

TEST(Session, TamperFails) {
  std::string secret="s";
  auto cookie = encode_cookie_value(secret, b64_encode("a=1"));
  cookie.back()='X';
  EXPECT_FALSE(decode_cookie_value(secret, cookie).has_value());
}

TEST(Session, ExtractCookie) {
  std::string h="foo=bar; examvan_session=abc.def; other=x";
  EXPECT_EQ(extract_cookie(h,"examvan_session"), "abc.def");
  EXPECT_EQ(extract_cookie(h,"missing"), "");
}

TEST(Session, VerifySession) {
  std::string secret="test-secret-1234567890abcdef";
  std::string payload=b64_encode("admin_id=42&username=guru&role=[\"guru\"]&is_super_admin=0");
  auto cv=encode_cookie_value(secret,payload);
  auto sess=verify_session_cookie(secret, cv);
  ASSERT_TRUE(sess.has_value());
  EXPECT_EQ(sess->admin_id,42);
  EXPECT_EQ(sess->username,"guru");
}
