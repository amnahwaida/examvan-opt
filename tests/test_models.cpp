#include <gtest/gtest.h>
#include "models/user.hpp"
#include "models/exam.hpp"
using namespace examvan::models;

TEST(Models, UsernameValid) {
  EXPECT_TRUE(is_valid_username("guru_01"));
  EXPECT_FALSE(is_valid_username("AB"));
  EXPECT_FALSE(is_valid_username("has space"));
  EXPECT_FALSE(is_valid_username("UPPER"));
}

TEST(Models, HasRole) {
  EXPECT_TRUE(has_role("[\"guru\",\"pengawas\"]","guru"));
  EXPECT_FALSE(has_role("[\"guru\"]","operator"));
}

TEST(Models, ExamDefaults) {
  Exam e;
  EXPECT_EQ(e.get_token_mode(),"dynamic");
  EXPECT_FALSE(e.is_active());
  e.status="active"; EXPECT_TRUE(e.is_active());
}

TEST(Models, ExamTokenMode) {
  Exam e;
  std::string m="static"; e.token_mode=m;
  EXPECT_EQ(e.get_token_mode(),"static");
}
