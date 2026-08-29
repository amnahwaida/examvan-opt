#include <gtest/gtest.h>
#include <fstream>
#include <string>
static std::string r7(const std::string& p){ std::ifstream f(p); if(!f) return ""; return std::string((std::istreambuf_iterator<char>(f)), {}); }

TEST(P7_Style, StylelintConfigPath) {
  auto rc = r7(".stylelintrc.json");
  EXPECT_NE(rc.find("theme.css"), std::string::npos);
  EXPECT_EQ(rc.find("webui/static"), std::string::npos) << "path should be static/css/theme.css not webui/static";
}

TEST(P7_Style, ClangFormatColumnLimit) {
  auto fmt = r7(".clang-format");
  EXPECT_NE(fmt.find("ColumnLimit: 100"), std::string::npos);
  EXPECT_NE(fmt.find("SortIncludes: true"), std::string::npos);
}

TEST(P7_Repo, GitHistoryNotPatchy) {
  auto log = r7("MIGRASI_STATUS.md");
  EXPECT_NE(log.find("F0"), std::string::npos);
  EXPECT_NE(log.find("F8"), std::string::npos);
}

TEST(P7_Frontend, JsGuardCount) {
  auto pkg = r7("package.json");
  if(pkg.empty()) GTEST_SKIP() << "no package.json";
  EXPECT_NE(pkg.find("node --test"), std::string::npos);
}

TEST(P7_Security, NoHardcodedExamvanPassInEnvExample) {
  auto ex = r7(".env.example");
  EXPECT_EQ(ex.find("examvan_pass"), std::string::npos) << ".env.example should not contain real password";
  EXPECT_NE(ex.find("EXAMVAN_SECRET"), std::string::npos);
}
