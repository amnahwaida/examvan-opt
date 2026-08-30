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

// TDD Pass 6: R2 mandatory — server refuses to start tanpa R2 credentials.
// Kontrak: Config::validate() harus throw jika r2_access_key/r2_secret_key/r2_endpoint kosong.
// Ini mencegah create_exam mencapai 503 R2_NOT_CONFIGURED — server langsung gagal di startup.

TEST(Config, ValidateThrowsWithoutR2) {
  Config c;
  c.secret_key="a-very-long-secret-key-for-validation";
  c.admin_user="admin";
  c.admin_pass="password123";
  // r2_access_key, r2_secret_key, r2_endpoint all empty (default) → harus throw
  EXPECT_THROW(c.validate(), std::runtime_error)
    << "Config::validate() harus menolak startup tanpa R2 credentials (mandatory)";
}

TEST(Config, ValidatePassesWithR2) {
  Config c;
  c.secret_key="a-very-long-secret-key-for-validation";
  c.admin_user="admin";
  c.admin_pass="password123";
  c.r2_access_key="AK_TEST_KEY";
  c.r2_secret_key="test-secret";
  c.r2_endpoint="https://test.r2.cloudflarestorage.com";
  EXPECT_NO_THROW(c.validate())
    << "Config::validate() harus lolos jika semua R2 credentials terisi";
}

TEST(Config, ValidatePartialR2Throws) {
  Config c;
  c.secret_key="a-very-long-secret-key-for-validation";
  c.admin_user="admin";
  c.admin_pass="password123";
  // Hanya access_key terisi — harus throw (partial R2 config)
  c.r2_access_key="AK_TEST_KEY";
  c.r2_secret_key="";
  c.r2_endpoint="";
  EXPECT_THROW(c.validate(), std::runtime_error)
    << "Config::validate() harus menolak jika R2 credentials tidak lengkap";
}
