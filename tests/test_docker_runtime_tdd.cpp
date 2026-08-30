#include <gtest/gtest.h>
#include <fstream>
#include <string>
static std::string rf(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)), {});
}
TEST(DockerRuntime, LibstdcxxCompatible){
  auto c=rf("Dockerfile");
  ASSERT_FALSE(c.empty());
  bool builder_is_gcc13 = c.find("FROM gcc:13")!=std::string::npos;
  size_t rt = c.find("AS runtime");
  ASSERT_NE(rt, std::string::npos);
  size_t line_start = c.rfind("\n", rt);
  size_t line_end = c.find("\n", rt);
  std::string runtime_line = c.substr(line_start==std::string::npos?0:line_start, line_end-line_start);
  bool runtime_is_gcc13 = runtime_line.find("gcc:13")!=std::string::npos;
  bool runtime_copies_lib = c.find("libstdc++")!=std::string::npos;
  bool ok = !builder_is_gcc13 || runtime_is_gcc13 || runtime_copies_lib;
  EXPECT_TRUE(ok) << "builder gcc:13 needs runtime gcc:13 or copy libstdc++ - runtime line: " << runtime_line << "\nfull: " << c;
}
