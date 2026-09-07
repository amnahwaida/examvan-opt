// Pass-20 M9-sisa: fallback sukses no-op saat DB down harus fail-closed (503),
// mengikuti pola set_approval (cek DATABASE_URL → 503 "Database tidak tersedia";
// memory/dev tanpa DATABASE_URL tetap 200 kompatibel unit test).
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_src26(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ===== delete_submission ====================================================

TEST(P26, DeleteSubmissionFallbackIsFailClosed){
  auto src = read_src26("src/handlers/admin/submissions.cpp");
  auto pos = src.find("Response delete_submission");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 4000);
  // Setelah blok HAS_LIBPQ, fallback tanpa PG wajib cek DATABASE_URL dan
  // mengembalikan 503 bila DB dikonfigurasi tapi tak terjangkau.
  EXPECT_NE(seg.find("DATABASE_URL"), std::string::npos)
    << "delete_submission harus membedakan prod (DATABASE_URL set) vs memory/dev";
  EXPECT_NE(seg.find("503"), std::string::npos)
    << "delete_submission harus 503 bila DB dikonfigurasi tapi down";
  // Komentar kontrak M9 agar pola tidak terhapus diam-diam.
  EXPECT_NE(seg.find("M9"), std::string::npos);
}

TEST(P26, DeleteSubmissionMemoryModeStill200){
  // Mode dev tanpa DATABASE_URL (unit test) tetap 200 kompatibel — pastikan
  // jalur 200 tanpa klaim eksplisit masih ada di handler.
  auto src = read_src26("src/handlers/admin/submissions.cpp");
  auto pos = src.find("Response delete_submission");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 4000);
  EXPECT_NE(seg.find("200"), std::string::npos)
    << "delete_submission harus tetap 200 di mode memory/dev tanpa DATABASE_URL";
}

// ===== get_auto_approve =====================================================

TEST(P26, GetAutoApproveFallbackIsFailClosed){
  auto src = read_src26("src/handlers/admin/pengawas.cpp");
  auto pos = src.find("Response get_auto_approve");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 3500);
  // PG down tidak boleh dilaporkan sebagai enabled=false yang valid: harus 503
  // bila DB dikonfigurasi (prod), seperti set_approval.
  EXPECT_NE(seg.find("DATABASE_URL"), std::string::npos)
    << "get_auto_approve harus membedakan prod (DATABASE_URL set) vs memory/dev";
  EXPECT_NE(seg.find("503"), std::string::npos)
    << "get_auto_approve harus 503 bila DB dikonfigurasi tapi down";
  EXPECT_NE(seg.find("M9"), std::string::npos);
}

TEST(P26, GetAutoApproveMemoryModeStill200){
  auto src = read_src26("src/handlers/admin/pengawas.cpp");
  auto pos = src.find("Response get_auto_approve");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 3500);
  EXPECT_NE(seg.find("200"), std::string::npos)
    << "get_auto_approve harus tetap 200 di mode memory/dev tanpa DATABASE_URL";
}

// ===== set_auto_approve (pola sama, sudah terindikasi tapi diverifikasi) ====

TEST(P26, SetAutoApproveFallbackIsFailClosed){
  auto src = read_src26("src/handlers/admin/pengawas.cpp");
  auto pos = src.find("Response set_auto_approve");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 3500);
  EXPECT_NE(seg.find("DATABASE_URL"), std::string::npos)
    << "set_auto_approve juga tak boleh klaim sukses saat DB down (pola M9)";
  EXPECT_NE(seg.find("503"), std::string::npos)
    << "set_auto_approve harus 503 bila DB dikonfigurasi tapi down";
  EXPECT_NE(seg.find("M9"), std::string::npos);
}
