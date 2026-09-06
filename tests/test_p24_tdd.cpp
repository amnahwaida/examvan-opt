#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
static std::string read_src24(const std::string& p){ std::ifstream f(p); if(!f) return ""; std::ostringstream ss; ss<<f.rdbuf(); return ss.str(); }
TEST(P24, DelegateValidatesPengawasIds){
  auto src = read_src24("src/handlers/admin/exams.cpp");
  auto pos = src.find("Response delegate_exam");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 4000);
  bool validates = seg.find("pengawas")!=std::string::npos &&
    (seg.find("SELECT")!=std::string::npos && seg.find("status")!=std::string::npos);
  EXPECT_TRUE(validates);
  EXPECT_NE(seg.find("Pengawas tidak valid"), std::string::npos);
}
TEST(P24, AuditLogHasInsert){
  auto src = read_src24("src/handlers/admin/pengawas.cpp");
  bool has = src.find("INSERT INTO admin_audit_logs")!=std::string::npos;
  if(!has) src = read_src24("src/handlers/admin/exams.cpp");
  if(src.find("INSERT INTO admin_audit_logs")==std::string::npos) src = read_src24("src/handlers/admin/submissions.cpp");
  EXPECT_NE(src.find("INSERT INTO admin_audit_logs"), std::string::npos);
}
TEST(P24, ApprovalFallbackIs503){
  auto src = read_src24("src/handlers/admin/pengawas.cpp");
  auto pos = src.find("Response set_approval");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 2500);
  EXPECT_NE(seg.find("503"), std::string::npos);
}
TEST(P24, VoucherInvalidIs400){
  auto src = read_src24("src/handlers/admin/vouchers.cpp");
  EXPECT_NE(src.find("__invalid__"), std::string::npos);
  auto pos = src.find("result==\"__invalid__\"");
  if(pos==std::string::npos) pos = src.find("__invalid__");
  auto seg = src.substr(pos, 600);
  EXPECT_NE(seg.find("400"), std::string::npos);
}
TEST(P24, ProtoSettingsNeverLeaksPassword){
  auto src = read_src24("src/handlers/admin/settings.cpp");
  auto pos = src.find("v1::Settings pb");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 800);
  EXPECT_EQ(seg.find("set_smtp_password"), std::string::npos);
}
