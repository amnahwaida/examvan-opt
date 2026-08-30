#include <gtest/gtest.h>
#include <fstream>
#include <string>
static std::string rf(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)), {});
}
TEST(DockerBuild, FilterExcludesInfraGuards){
  auto c=rf("Dockerfile");
  ASSERT_FALSE(c.empty()) << "Dockerfile not found";
  auto pos=c.find("gtest_filter");
  ASSERT_NE(pos, std::string::npos);
  std::string seg=c.substr(pos, 700);
  EXPECT_NE(seg.find("Review_"), std::string::npos) << "builder gtest_filter should exclude Review_* (file-existence guards) : " << seg;
  EXPECT_NE(seg.find("R3_"), std::string::npos) << "builder should exclude R3_* : " << seg;
  EXPECT_NE(seg.find("R4_"), std::string::npos) << "builder should exclude R4_* (needs .github) : " << seg;
  EXPECT_NE(seg.find("R5_"), std::string::npos) << "builder should exclude R5_* : " << seg;
}
TEST(DockerBuild, BuilderCopiesRequiredFiles){
  auto c=rf("Dockerfile");
  bool has_dockerfile_copy = c.find("COPY Dockerfile")!=std::string::npos;
  bool has_github_copy = c.find(".github")!=std::string::npos;
  bool filter_excludes = c.find("Review_")!=std::string::npos;
  EXPECT_TRUE(has_dockerfile_copy || filter_excludes) << "Either COPY Dockerfile or exclude Review_Dockerfile via filter";
  (void)has_github_copy;
}
