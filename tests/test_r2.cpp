#include <gtest/gtest.h>
#include "handlers/r2/r2.hpp"
#include "services/examtoken/examtoken.hpp"
#include "helpers/utils.hpp"
using namespace examvan;

TEST(R2, PresignEnabled) {
  r2::R2Config c{"k","s","https://ep","b"};
  EXPECT_TRUE(c.enabled());
  EXPECT_NE(r2::presign_url(c,"key.pdf").find("b/key.pdf"), std::string::npos);
  r2::R2Config c2{"","","",""}; EXPECT_FALSE(c2.enabled());
}

TEST(R2, Keys) {
  EXPECT_EQ(r2::object_key_for_exam(5,"a.pdf"),"exams/5/a.pdf");
  EXPECT_NE(r2::object_key_for_app("2.7.2","student").find("2.7.2"), std::string::npos);
}

TEST(ExamToken, Hash) {
  auto h=examtoken::hash_token("ABC");
  EXPECT_EQ(h.size(), 64u);
  EXPECT_TRUE(examtoken::verify_token("ABC", h));
  EXPECT_FALSE(examtoken::verify_token("ABD", h));
}

TEST(Helpers, TokenGen) {
  auto t=helpers::generate_token(8);
  EXPECT_EQ(t.size(), 8u);
  EXPECT_TRUE(helpers::is_valid_exam_token(t));
  EXPECT_FALSE(helpers::is_valid_exam_token("bad token"));
}
