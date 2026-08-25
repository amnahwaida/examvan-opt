#include <gtest/gtest.h>
#include "session/csrf.hpp"
using namespace examvan;
TEST(CSRF, GenerateAndVerify) {
  auto t=generate_csrf_token();
  EXPECT_FALSE(t.empty());
  EXPECT_TRUE(verify_csrf(t,t));
  EXPECT_FALSE(verify_csrf(t, t+"x"));
  EXPECT_FALSE(verify_csrf("",""));
}
