// Pass-24 TDD remediasi Batch 2 (MEDIUM) — RED phase.
// Temuan: docs/review-temuan-2026-09-09-pass24.md
//  - G2 : hasil requeue() diabaikan → job hilang tanpa jejak saat Redis blip
//  - G1/G9/E-c/F5/D5 : keluarga stack-local pool (satu kelas, lintas file)
//  - G3 : PQconnectdb dieksekusi sambil memegang mutex pool + tanpa timeout
//  - G4 : pg_conninfo_from_url memaksa sslmode=require, membuang parameter lain
//  - D1 : create_exam idempotency reservation bocor di 3 jalur error protobuf
//  - F7 : R2Config::enabled() tanpa cek bucket → presign bucket kosong
//  - I2 : limit_req login menghantam GET (DoS ringan NAT/kuota IP)
//  - I3 : .env ikut build context Docker (kredensial produksi)
//
// Kontrak (RED dulu, GREEN setelah remediasi):
//  1. G2  — checked_requeue: LPUSH gagal → push_failed + JobResult (jejak).
//  2. G1  — submission_queue.cpp memakai pool proses-wide (bukan per batch).
//  3. G9  — main.cpp migrate/hydrate memakai global_pool (satu pool proses).
//  4. E-c — export.cpp memakai pool proses-wide.
//  5. F5  — api/exams.cpp, public/hasil.cpp, router_full.cpp memakai pool global.
//  6. D5  — admin/exams.cpp + pengawas.cpp tidak membangun pool stack-lokal.
//  7. G3  — acquire() TIDAK memanggil PQconnectdb di bawah mutex; conninfo
//           punya connect_timeout.
//  8. G4  — query-string PG diparse utuh: sslmode/connect_timeout dipertahankan,
//           default `require` hanya bila sslmode absen.
//  9. D1  — 3 jalur error protobuf create_exam me-release reservation idempotency.
// 10. F7  — enabled() wajib menuntut bucket + endpoint lengkap.
// 11. I2  — rate-limit login hanya untuk POST (limit_except POST).
// 12. I3  — .dockerignore ada dan mengecualikan .env.
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>

#include "db/pool.hpp"
#include "handlers/r2/r2.hpp"

using namespace examvan;

namespace {

std::string read_src34(const std::string& p) {
  std::ifstream f(p);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string between(const std::string& src, const std::string& begin_marker,
                    const std::string& end_marker) {
  auto b = src.find(begin_marker);
  if (b == std::string::npos) return "";
  auto e = src.find(end_marker, b + begin_marker.size());
  return e == std::string::npos ? src.substr(b) : src.substr(b, e - b);
}

int count_occurrences(const std::string& hay, const std::string& needle) {
  int n = 0;
  size_t p = 0;
  while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
  return n;
}

}  // namespace

// G2: hasil requeue() wajib diperiksa — LPUSH gagal → push_failed + JobResult.
TEST(P34, G2_RequeueChecked) {
  auto src = read_src34("src/queue/submission_queue.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_NE(src.find("checked_requeue"), std::string::npos)
      << "wajib ada helper checked_requeue (LPUSH gagal → failed-queue + JobResult)";
  // Hanya helper yang boleh memanggil requeue mentah (1 panggilan internal).
  EXPECT_LE(count_occurrences(src, "queue_->requeue("), 1)
      << "semua call-site requeue wajib lewat checked_requeue";
  EXPECT_NE(src.find("push_failed"), std::string::npos)
      << "LPUSH gagal wajib didorong ke failed-queue";
  EXPECT_NE(src.find("Gagal mengantar ulang"), std::string::npos)
      << "wajib ada JobResult jejak kegagalan antar-ulang";
}

// G1: run_batch wajib memakai pool proses-wide (bukan pool stack per batch).
TEST(P34, G1_QueueUsesGlobalPool) {
  auto src = read_src34("src/queue/submission_queue.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(src.find("DbPool pool("), std::string::npos)
      << "run_batch tidak boleh membangun DbPool stack-lokal per iterasi";
  EXPECT_EQ(src.find("RealPool real("), std::string::npos)
      << "run_batch tidak boleh membangun RealPool stack-lokal per iterasi";
  EXPECT_NE(src.find("with_global_pg"), std::string::npos)
      << "run_batch wajib memakai pool proses-wide (with_global_pg/global_pool)";
}

// G9: main.cpp migrate/hydrate memakai pool global — satu pool proses-wide.
TEST(P34, G9_MainUsesGlobalPool) {
  auto src = read_src34("src/main.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(src.find("RealPool db("), std::string::npos)
      << "main tidak boleh membangun pool stack terpisah dari global_pool";
  EXPECT_NE(src.find("global_pool"), std::string::npos)
      << "main wajib menginisialisasi/memakai db::global_pool";
}

// E-c: export xlsx memakai pool proses-wide.
TEST(P34, Ec_ExportUsesGlobalPool) {
  auto src = read_src34("src/handlers/admin/export.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(src.find("RealPool real("), std::string::npos);
  EXPECT_EQ(src.find("DbPool pool("), std::string::npos);
  EXPECT_NE(src.find("with_global_pg"), std::string::npos);
}

// F5: api/exams, hasil, router_full memakai pool proses-wide.
TEST(P34, F5_ApiPublicRouterUseGlobalPool) {
  for (const char* f : {"src/handlers/api/exams.cpp", "src/handlers/public/hasil.cpp",
                        "src/http/router_full.cpp"}) {
    auto src = read_src34(f);
    ASSERT_FALSE(src.empty()) << f;
    EXPECT_EQ(src.find("RealPool real("), std::string::npos)
        << f << " tidak boleh membangun RealPool stack-lokal";
    EXPECT_EQ(src.find("DbPool pool("), std::string::npos)
        << f << " tidak boleh membangun DbPool stack-lokal";
    EXPECT_NE(src.find("with_global_pg"), std::string::npos)
        << f << " wajib memakai with_global_pg/global_pool";
  }
}

// D5: admin exams + pengawas tidak membangun pool stack-lokal (sisa anggota
// keluarga stack-pool di area D).
TEST(P34, D5_AdminExamsPengawasUseGlobalPool) {
  for (const char* f : {"src/handlers/admin/exams.cpp", "src/handlers/admin/pengawas.cpp"}) {
    auto src = read_src34(f);
    ASSERT_FALSE(src.empty()) << f;
    EXPECT_EQ(src.find("RealPool real("), std::string::npos)
        << f << " tidak boleh membangun RealPool stack-lokal";
    EXPECT_EQ(src.find("DbPool pool("), std::string::npos)
        << f << " tidak boleh membangun DbPool stack-lokal";
  }
}

// auth_store produksi ×6 memakai pool global (bukan pool 2-koneksi per op).
TEST(P34, AuthStoreUsesGlobalPool) {
  auto src = read_src34("src/handlers/auth/auth_store.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(src.find("RealPool("), std::string::npos)
      << "auth_store tidak boleh membangun RealPool stack-lokal per operasi";
  EXPECT_NE(src.find("global_pool"), std::string::npos)
      << "auth_store wajib memakai db::global_pool";
}

// G3: PQconnectdb di luar mutex pool + connect_timeout di conninfo.
TEST(P34, G3_ConnectOutsideMutex) {
  auto src = read_src34("src/db/pool_real.cpp");
  ASSERT_FALSE(src.empty());
  auto acquire_win = between(src, "PgConnPtr RealPool::acquire()",
                             "void RealPool::release");
  ASSERT_FALSE(acquire_win.empty()) << "RealPool::acquire tidak ditemukan";
  EXPECT_EQ(acquire_win.find("PQconnectdb"), std::string::npos)
      << "acquire() wajib TIDAK memanggil PQconnectdb sambil memegang mutex";
  EXPECT_NE(src.find("connect_timeout"), std::string::npos)
      << "conninfo pool wajib punya connect_timeout (PG lambat jangan membekukan pool)";
}

// G4: query-string PG diparse utuh — sslmode klien dihormati, default require
// hanya bila sslmode absen; parameter lain (connect_timeout dll) dipertahankan.
TEST(P34, G4_PgConninfoQueryString) {
  // verify-full wajib dipertahankan (dulu dipaksa require → MITM tak terdeteksi).
  auto ci1 = pg_conninfo_from_url("postgresql://u:p@db.example:5432/examvan?sslmode=verify-full");
  EXPECT_NE(ci1.find("sslmode=verify-full"), std::string::npos)
      << "sslmode klien wajib dipertahankan apa adanya: " << ci1;
  // disable untuk PG lokal no-TLS wajib dipertahankan (dulu dipaksa require).
  auto ci2 = pg_conninfo_from_url("postgresql://u:p@127.0.0.1:5432/examvan?sslmode=disable");
  EXPECT_NE(ci2.find("sslmode=disable"), std::string::npos) << ci2;
  EXPECT_EQ(ci2.find("sslmode=require"), std::string::npos) << ci2;
  // Parameter non-sslmode wajib diteruskan, bukan dibuang.
  auto ci3 = pg_conninfo_from_url("postgresql://u:p@db:5432/examvan?connect_timeout=7&application_name=examvan");
  EXPECT_NE(ci3.find("connect_timeout=7"), std::string::npos) << ci3;
  EXPECT_NE(ci3.find("application_name=examvan"), std::string::npos) << ci3;
  // Tanpa query-string sslmode → default aman `require`.
  auto ci4 = pg_conninfo_from_url("postgresql://u:p@db:5432/examvan");
  EXPECT_NE(ci4.find("sslmode=require"), std::string::npos) << ci4;
}

// D1: reservation idempotency wajib di-release di 3 jalur error protobuf.
TEST(P34, D1_IdempotencyReleaseOnProtobufErrors) {
  auto src = read_src34("src/handlers/admin/exams.cpp");
  ASSERT_FALSE(src.empty());
  // (a) require_protobuf gagal (415 PROTOBUF_REQUIRED).
  auto w1 = between(src, "if(auto err = middleware::require_protobuf(req, cfg_pb); err)",
                    "std::string name, fpath, sz, custom;");
  ASSERT_FALSE(w1.empty()) << "jalur require_protobuf tidak ditemukan";
  EXPECT_NE(w1.find("release_and_return"), std::string::npos)
      << "415 protobuf-required wajib release reservation";
  // (b) ParseFromArray gagal (400 invalid protobuf).
  auto w2 = between(src, "if(!pb.ParseFromArray(req.body.data(), req.body.size()))",
                    "name = pb.name();");
  ASSERT_FALSE(w2.empty()) << "jalur parse-protobuf tidak ditemukan";
  EXPECT_NE(w2.find("release_and_return"), std::string::npos)
      << "400 invalid protobuf wajib release reservation";
  // (c) build tanpa protobuf (415 PROTOBUF_REQUIRED pada #else).
  auto w3 = between(src, "Response r; r.status=415; r.json(415,\"{\\\"error\\\":\\\"protobuf not enabled\\\"",
                    "} else {");
  ASSERT_FALSE(w3.empty()) << "jalur no-protobuf tidak ditemukan";
  EXPECT_NE(w3.find("release_and_return"), std::string::npos)
      << "415 no-protobuf wajib release reservation";
}

// F7: R2Config::enabled() wajib menuntut konfigurasi LENGKAP (termasuk bucket).
TEST(P34, F7_R2EnabledRequiresBucket) {
  const r2::R2Config no_bucket{"ak", "sk", "https://ep.example", ""};
  const r2::R2Config no_access{"", "sk", "https://ep.example", "bucket"};
  const r2::R2Config no_secret{"ak", "", "https://ep.example", "bucket"};
  const r2::R2Config no_endpoint{"ak", "sk", "", "bucket"};
  const r2::R2Config full{"ak", "sk", "https://ep.example", "bucket"};
  // access+secret+endpoint tanpa bucket → TIDAK enabled (dulu true → presign ke bucket "").
  EXPECT_FALSE(no_bucket.enabled());
  // Partial config mana pun → tidak enabled.
  EXPECT_FALSE(no_access.enabled());
  EXPECT_FALSE(no_secret.enabled());
  EXPECT_FALSE(no_endpoint.enabled());
  // Lengkap → enabled.
  EXPECT_TRUE(full.enabled());
}

// I2: rate-limit login wajib POST-only (GET login page tidak menghabiskan kuota).
TEST(P34, I2_LoginRateLimitPostOnly) {
  auto src = read_src34("nginx/nginx.conf");
  ASSERT_FALSE(src.empty());
  auto seg = between(src, "location ~ ^/(login|admin/login)", "location /api/health");
  ASSERT_FALSE(seg.empty()) << "location login tidak ditemukan";
  EXPECT_NE(seg.find("limit_except POST"), std::string::npos)
      << "limit_req login wajib dibungkus limit_except POST";
}

// I3: .env wajib dikecualikan dari build context Docker.
TEST(P34, I3_DockerignoreExcludesEnv) {
  std::ifstream f(".dockerignore");
  ASSERT_TRUE(f.good()) << ".dockerignore wajib ada di root repo";
  std::ostringstream ss; ss << f.rdbuf();
  auto s = ss.str();
  EXPECT_NE(s.find(".env"), std::string::npos)
      << ".dockerignore wajib mengecualikan .env (kredensial produksi)";
}
