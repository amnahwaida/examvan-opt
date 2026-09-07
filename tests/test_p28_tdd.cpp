// Pass-20 B1: audit-log tanpa atribusi — write_audit_log selalu INSERT
// user_id=NULL, username='' sehingga log tak bisa menjawab "siapa yang
// melakukan aksi". Kontrak (semua best-effort, tak menggagalkan aksi utama):
// 1) INSERT admin_audit_logs tidak boleh memakai NULL hardcoded untuk user_id
//    atau '' hardcoded untuk username — harus dari X-Internal-Admin-* headers.
// 2) Router meneruskan X-Internal-Admin-Username (dari session terverifikasi).
// 3) Cakupan audit: set_approval (sudah ada), set_auto_approve, bulk-toggle,
//    bulk-delete, system-app upload/delete.
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_src28(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

TEST(P28, RouterForwardsAdminUsernameHeader){
  // SessionData.username ada — router wajib meneruskannya agar handler bisa
  // menulis atribusi audit yang benar (bukan username kosong).
  auto src = read_src28("src/http/router_full.cpp");
  EXPECT_NE(src.find("X-Internal-Admin-Username"), std::string::npos)
    << "router harus meneruskan username session via X-Internal-Admin-Username";
}

TEST(P28, AuditInsertNeverHardcodesAnonymous){
  // INSERT audit di semua handler wajib memakai parameter (bukan NULL,''
  // hardcoded) sehingga atribusi diisi dari identity header.
  auto src = read_src28("src/handlers/admin/pengawas.cpp");
  EXPECT_EQ(src.find("VALUES (NULLIF($1,'')::int,NULL,$2,$3,$4)"), std::string::npos)
    << "write_audit_log tidak boleh hardcode user_id=NULL / username=''";
  EXPECT_NE(src.find("audit_identity_from"), std::string::npos)
    << "butuh helper audit_identity_from(req) untuk atribusi dari X-Internal headers";
  auto exams = read_src28("src/handlers/admin/exams.cpp");
  EXPECT_NE(exams.find("audit_identity_from"), std::string::npos)
    << "exams.cpp butuh helper audit_identity_from untuk bulk ops";
}

TEST(P28, SetApprovalAuditCarriesIdentity){
  auto src = read_src28("src/handlers/admin/pengawas.cpp");
  auto pos = src.find("Response set_approval");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 4200);
  EXPECT_NE(seg.find("audit_identity_from"), std::string::npos)
    << "set_approval harus menulis audit dengan identitas caller";
}

TEST(P28, SetAutoApproveHasAudit){
  auto src = read_src28("src/handlers/admin/pengawas.cpp");
  auto pos = src.find("Response set_auto_approve");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 3600);
  EXPECT_NE(seg.find("write_audit_log"), std::string::npos)
    << "set_auto_approve wajib menulis audit (aksi admin tercatat)";
  EXPECT_NE(seg.find("audit_identity_from"), std::string::npos)
    << "audit set_auto_approve harus beratribusi ke caller";
}

TEST(P28, BulkOpsHaveAudit){
  auto exams = read_src28("src/handlers/admin/exams.cpp");
  for(const char* fn : {"Response bulk_toggle_exams", "Response bulk_delete_exams"}){
    auto pos = exams.find(fn);
    ASSERT_NE(pos, std::string::npos) << fn << " tidak ditemukan";
    auto seg = exams.substr(pos, 5200);
    EXPECT_NE(seg.find("write_audit_log"), std::string::npos)
      << fn << " wajib menulis audit (destructive, harus tercatat siapa)";
    EXPECT_NE(seg.find("audit_identity_from"), std::string::npos)
      << fn << " audit harus beratribusi ke caller";
  }
}

TEST(P28, SystemAppMutationsHaveAudit){
  auto src = read_src28("src/handlers/admin/settings.cpp");
  auto pos = src.find("Response system_apps_page");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 7200);
  // Aksi mutasi: upload (INSERT system_apps) dan delete (DELETE + R2 remove).
  EXPECT_NE(seg.find("write_audit_log"), std::string::npos)
    << "system-app upload/delete wajib menulis audit (upload 100MB + R2 cleanup)";
  EXPECT_NE(seg.find("audit_identity_from"), std::string::npos)
    << "audit system-app harus beratribusi ke caller";
}
