#include <gtest/gtest.h>
#include "utils/sanitize.hpp"
using namespace examvan;

TEST(Sanitize, StripsHtmlMeta) {
  EXPECT_EQ(sanitize_ws_field("<script>alert", 100), "scriptalert");
  EXPECT_EQ(sanitize_ws_field("a&b=c\"d'e`", 100), "abcde");
}

TEST(Sanitize, StripsControl) {
  std::string s; s.push_back(0x01); s+="hello"; s.push_back(0x7f);
  EXPECT_EQ(sanitize_ws_field(s, 100), "hello");
}

TEST(Sanitize, CapsLength) {
  EXPECT_EQ(sanitize_ws_field("abcdefgh", 4), "abcd");
}

TEST(Sanitize, MacTrimsAndCaps) {
  EXPECT_EQ(sanitize_ws_mac("  abc  "), "abc");
  std::string long_mac(150,'x');
  EXPECT_EQ(sanitize_ws_mac(long_mac).size(), 100u);
}

TEST(Sanitize, MacStripsControl) {
  std::string s; s.push_back(0x01); s+="aa:bb"; s.push_back(0x1f);
  EXPECT_EQ(sanitize_ws_mac(s), "aa:bb");
}
