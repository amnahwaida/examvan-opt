#include <gtest/gtest.h>
#include <fstream>
#include <string>
static std::string r6(const std::string& p){ std::ifstream f(p); if(!f) return ""; return std::string((std::istreambuf_iterator<char>(f)), {}); }

TEST(P6_Repo, InternalShouldBeArchived) {
  auto g = r6(".gitignore");
  EXPECT_NE(g.find("internal"), std::string::npos) << "internal/ Go legacy should be gitignored or moved to vendor/go-legacy";
}

TEST(P6_Repo, VcpkgComplete) {
  auto v = r6("vcpkg.json");
  EXPECT_NE(v.find("libcurl"), std::string::npos) << "vcpkg should include libcurl";
  EXPECT_NE(v.find("zlib"), std::string::npos);
  EXPECT_NE(v.find("openssl"), std::string::npos);
}

TEST(P6_Style, ClangFormatStrict) {
  auto ci = r6(".github/workflows/ci.yml");
  EXPECT_EQ(ci.find("|| true"), std::string::npos) << "clang-format must be blocking";
  auto fmt = r6(".clang-format");
  EXPECT_NE(fmt.find("ColumnLimit"), std::string::npos);
  EXPECT_NE(fmt.find("SortIncludes"), std::string::npos);
}

TEST(P6_Style, StylelintActive) {
  auto rc = r6(".stylelintrc.json");
  EXPECT_NE(rc.size(), 0u) << ".stylelintrc.json should exist";
  auto css = r6("static/css/theme.css");
  EXPECT_NE(css.size(), 0u);
}

TEST(P6_Repo, SoakLogsIgnored) {
  auto gi = r6(".gitignore");
  EXPECT_NE(gi.find("soak-logs"), std::string::npos);
  EXPECT_NE(gi.find("build"), std::string::npos);
}

TEST(P6_Build, CcacheEnabled) {
  auto cm = r6("CMakeLists.txt");
  EXPECT_NE(cm.find("ccache"), std::string::npos) << "should use ccache for faster docker builds";
}
