#include <gtest/gtest.h>
#include "handlers/admin/users.hpp"
using namespace examvan::handlers::admin;

TEST(F6Users, CreateRequiresUsername) {
  examvan::Request req; req.body="password=12345678";
  auto r=create_user(req); EXPECT_EQ(r.status,400);
}

TEST(F6Users, CreateInvalidUsername) {
  examvan::Request req; req.body="username=AB&password=12345678";
  auto r=create_user(req); EXPECT_EQ(r.status,400);
  EXPECT_NE(r.body.find("username"), std::string::npos);
}

TEST(F6Users, CreatePasswordMin8) {
  examvan::Request req; req.body="username=guru_01&password=short";
  auto r=create_user(req); EXPECT_EQ(r.status,400);
  EXPECT_NE(r.body.find("8"), std::string::npos);
}

TEST(F6Users, CreateSuccess201) {
  examvan::Request req; req.body="username=guru_01&password=12345678&role=guru";
  auto r=create_user(req); EXPECT_EQ(r.status,201);
  EXPECT_NE(r.body.find("\"id\""), std::string::npos);
}

TEST(F6Users, CreateOperatorRequiresSuperadmin) {
  examvan::Request req; req.body="username=op_01&password=12345678&role=operator";
  req.headers["X-Role"]="guru";
  auto r=create_user(req); EXPECT_EQ(r.status,403);
}
