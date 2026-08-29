#include <gtest/gtest.h>
#include <fstream>
#include <string>
static std::string r11(const std::string& p){ std::ifstream f(p); if(!f) return ""; return std::string((std::istreambuf_iterator<char>(f)), {}); }

TEST(P11_Router, NoDuplicateSaaSSettings) {
  auto s = r11("src/http/router_full.cpp");
  std::string needle="\"GET\",\"/admin/api/saas-settings\"";
  size_t first = s.find(needle);
  ASSERT_NE(first, std::string::npos);
  size_t second = s.find(needle, first+1);
  EXPECT_EQ(second, std::string::npos) << "duplicate GET saas-settings should be removed";
}

TEST(P11_Security, NoXRoleHeader) {
  auto s = r11("src/http/router_full.cpp");
  EXPECT_EQ(s.find("X-Role"), std::string::npos);
  auto a = r11("src/middleware/auth.cpp");
  EXPECT_NE(a.find("is_authenticated"), std::string::npos);
}

TEST(P11_Db, SchemaUsesTombstone) {
  auto sch = r11("migrasi-cpp/03-peta-modul-dan-kontrak-api.md");
  if(sch.empty()) sch = r11("internal/database/schema.sql");
  EXPECT_NE(sch.find("tombstone"), std::string::npos);
}
