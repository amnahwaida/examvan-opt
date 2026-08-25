#include <gtest/gtest.h>
#include "config/config.hpp"
#include <cstdlib>
using namespace examvan;

TEST(Config, LoadDefaults) {
  unsetenv("PORT"); unsetenv("DATABASE_URL");
  auto c=Config::load();
  EXPECT_EQ(c.port,5000);
  EXPECT_EQ(c.version,"2.7.2");
}

TEST(Config, LoadFromEnv) {
  setenv("PORT","8080",1);
  setenv("DATABASE_URL","postgresql://x",1);
  auto c=Config::load();
  EXPECT_EQ(c.port,8080);
  EXPECT_EQ(c.database_url,"postgresql://x");
  unsetenv("PORT"); unsetenv("DATABASE_URL");
}

TEST(Config, ValidateRequiresSecret) {
  Config c; c.secret_key=""; c.admin_user="a"; c.admin_pass="b";
  c.r2_access_key="k"; c.r2_secret_key="s"; c.r2_endpoint="e";
  EXPECT_THROW(c.validate(), std::runtime_error);
}
