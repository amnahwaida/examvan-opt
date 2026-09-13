// P36 — kontrak TDD Batch 3 (temuan LOW) review pass-24.
// Daftar temuan yang dikunci test ini:
//  1. D6  — wib_to_utc_iso ketat: full-consumption + hari-dalam-bulan valid.
//  2. D7  — audit-log lengkap di create/update/delete/delegate/save_questions.
//  3. D8  — UPDATE 0-baris set_approval/set_auto_approve → 404 (bukan 200).
//  4. D9  — pengawas_state dari data live (SELECT count), bukan stub hardcoded.
//  5. D10 — komentar exam_bulk_scope_ok mencerminkan perilaku fail-closed.
//  6. D11 — user_id=0 disimpan NULL (bukan 0) di exams.cpp & pengawas.cpp.
//  7. D12 — predikat dashboard mencakup anggota exam_pengawas.
//  8. D13 — submissions dashboard dari data live (bukan hardcoded 0).
//  9. E-b — export_submissions_csv: di-wire atau dihapus (bukan dead code).
// 10. E-d — early-return lambda submissions me-release koneksi (anti-leak).
// 11. G5  — stop() tanpa sleep backoff per-job (200s shutdown dihapus).
// 12. G6  — Redis AUTH username+password & connect timeout (managed ACL).
// 13. G7  — BEGIN drain_heartbeat_batch diperiksa (requeue saat gagal).
// 14. G8  — enqueue_job_to_redis pakai koneksi thread-local (bukan baru/submit).
// 15. G10 — test_pg_integration: RAII cleanup + restore saas_settings.
// 16. F8  — R2 verify/HEAD punya connect timeout (anti-hang 15s).
// 17. F11 — submit rate-limit key pakai X-Real-IP (anti-evasi XFF spoof).
// 18. I5  — endpoint /api/health/ready probe dependency (live ≠ ready).
// 19. Dead-code — middleware::version_gate ter-wire di router (bukan mati).
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <optional>

#include "helpers/utils.hpp"

using namespace examvan;

namespace {

std::string read_src36(const std::string& p) {
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

std::string between_last(const std::string& src, const std::string& begin_marker,
                         const std::string& end_marker) {
  auto b = src.rfind(begin_marker);
  if (b == std::string::npos) return "";
  auto e = src.find(end_marker, b + begin_marker.size());
  return e == std::string::npos ? src.substr(b) : src.substr(b, e - b);
}

int count_of(const std::string& hay, const std::string& needle) {
  int n = 0;
  size_t p = 0;
  while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
  return n;
}

}  // namespace

// ===== D6: parser WIB ketat ==============================================

TEST(P36, D6_WibToUtcIsoStrict) {
  // Full-consumption: sampah setelah menit wajib ditolak (dulu sscanf tanpa %n).
  EXPECT_FALSE(helpers::wib_to_utc_iso("2026-05-01 10:00JUNK").has_value());
  EXPECT_FALSE(helpers::wib_to_utc_iso("2026-05-01 10:00 ").has_value()) << "trailing space ditolak";
  // Hari-dalam-bulan divalidasi: 2026 bukan tahun kabisat.
  EXPECT_FALSE(helpers::wib_to_utc_iso("2026-02-30 10:00").has_value());
  EXPECT_FALSE(helpers::wib_to_utc_iso("2026-04-31 23:59").has_value());
  EXPECT_FALSE(helpers::wib_to_utc_iso("2026-13-01 10:00").has_value());
  EXPECT_FALSE(helpers::wib_to_utc_iso("2026-01-32 10:00").has_value());
  // Kabisat valid tetap diterima.
  EXPECT_TRUE(helpers::wib_to_utc_iso("2028-02-29 07:00").has_value());
  // Format valid & konversi WIB(+7) → UTC benar.
  auto v = helpers::wib_to_utc_iso("2026-05-01 10:00");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, "2026-05-01T03:00:00Z");
  // Detik opsional tetap diterima (format input lama ada yang menyertakan :ss).
  auto v2 = helpers::wib_to_utc_iso("2026-05-01 10:00:30");
  ASSERT_TRUE(v2.has_value());
  EXPECT_EQ(*v2, "2026-05-01T03:00:00Z");
}

// ===== D7: audit-log lengkap =============================================

TEST(P36, D7_AuditLogCoverage) {
  auto src = read_src36("src/handlers/admin/exams.cpp");
  ASSERT_FALSE(src.empty());
  // Lima handler mutasi yang dulu tanpa audit wajib memanggil write_audit_log.
  EXPECT_NE(count_of(between(src, "Response create_exam(const Request& req){",
                             "Response update_exam(const Request& req){"),
                     "write_audit_log("), 0)
      << "create_exam wajib menulis audit log";
  EXPECT_GE(count_of(between(src, "Response update_exam(const Request& req){",
                             "Response delete_exam(const Request& req){"),
                     "write_audit_log("), 1)
      << "update_exam (edit/token/regenerate) wajib menulis audit log";
  EXPECT_NE(count_of(between(src, "Response delete_exam(const Request& req){",
                             "Response save_exam_questions(const Request& req){"),
                     "write_audit_log("), 0)
      << "delete_exam wajib menulis audit log";
  EXPECT_NE(count_of(between(src, "Response save_exam_questions(const Request& req){",
                             "Response bulk_toggle_exams(const Request& req){"),
                     "write_audit_log("), 0)
      << "save_exam_questions wajib menulis audit log";
  EXPECT_NE(count_of(between_last(src, "Response delegate_exam(const Request& req){", "\n}"),
                     "write_audit_log("), 0)
      << "delegate_exam wajib menulis audit log";
}

// ===== D8: UPDATE 0-baris → 404 ==========================================

TEST(P36, D8_ZeroRowUpdateReturns404) {
  auto src = read_src36("src/handlers/admin/pengawas.cpp");
  ASSERT_FALSE(src.empty());
  auto w1 = between(src, "Response set_approval(const Request& req){",
                    "Response get_auto_approve(const Request& req){");
  ASSERT_FALSE(w1.empty());
  EXPECT_NE(w1.find("PQcmdTuples"), std::string::npos)
      << "set_approval wajib memeriksa jumlah baris UPDATE (0 baris → 404, bukan 200)";
  auto w2 = between(src, "Response set_auto_approve(const Request& req){",
                    "Response exam_audit_logs(const Request& req){");
  ASSERT_FALSE(w2.empty());
  EXPECT_NE(w2.find("PQcmdTuples"), std::string::npos)
      << "set_auto_approve wajib memeriksa jumlah baris UPDATE (0 baris → 404)";
}

// ===== D9: pengawas_state dari data live =================================

TEST(P36, D9_PengawasStateLive) {
  auto src = read_src36("src/handlers/admin/pengawas.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between_last(src, "Response pengawas_state(const Request& req){", "\n}");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("SELECT"), std::string::npos)
      << "pengawas_state wajib menghitung state live dari DB, bukan stub";
  EXPECT_NE(w.find("count("), std::string::npos)
      << "active_exams/online_students/pending_approvals wajib hasil query count";
}

// ===== D10: komentar mencerminkan fail-closed ============================

TEST(P36, D10_CommentMatchesFailClosed) {
  auto src = read_src36("src/handlers/admin/exams.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "exam_bulk_scope_ok", "Response");
  ASSERT_FALSE(w.empty());
  EXPECT_EQ(w.find("fail-open"), std::string::npos)
      << "komentar klaim fail-open padahal kode fail-closed — perbaiki komentar";
  EXPECT_NE(w.find("fail-closed"), std::string::npos)
      << "komentar wajib mendokumentasikan perilaku fail-closed yang sebenarnya";
}

// ===== D11: user_id=0 → NULL =============================================

TEST(P36, D11_UserIdZeroSavedAsNull) {
  auto src = read_src36("src/handlers/admin/exams.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_NE(src.find("? std::to_string(user_id)"), std::string::npos)
      << "exams.cpp: user_id>0 ? to_string : '' agar NULLIF menyimpan NULL, bukan 0";
  auto src2 = read_src36("src/handlers/admin/pengawas.cpp");
  ASSERT_FALSE(src2.empty());
  EXPECT_NE(src2.find("? std::to_string(user_id)"), std::string::npos)
      << "pengawas.cpp: user_id>0 ? to_string : '' agar NULLIF menyimpan NULL";
}

// ===== D12: dashboard mencakup exam_pengawas =============================

TEST(P36, D12_DashboardIncludesExamPengawas) {
  auto src = read_src36("src/handlers/admin/dashboard.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_NE(count_of(src, "exam_pengawas"), 0)
      << "predikat dashboard (page+stats) wajib mencakup ujian yang ditugaskan "
         "via exam_pengawas — paritas exam_access router";
}

// ===== D13: submissions dashboard dari data live =========================

TEST(P36, D13_DashboardSubmissionsNotHardcoded) {
  auto src = read_src36("src/handlers/admin/dashboard.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(src.find("\\\"submissions\\\":0}"), std::string::npos)
      << "submissions dashboard wajib dari data live, bukan hardcoded 0";
  EXPECT_NE(src.find("pending_submissions"), std::string::npos)
      << "hitung submissions live dari store";
}

// ===== E-b: export csv wired atau dihapus ================================

TEST(P36, Eb_ExportCsvWiredOrRemoved) {
  auto decl = read_src36("src/handlers/admin/export.hpp");
  auto impl = read_src36("src/handlers/admin/export.cpp");
  auto router = read_src36("src/http/router_full.cpp");
  ASSERT_FALSE(router.empty());
  const bool declared = decl.find("export_submissions_csv") != std::string::npos;
  const bool defined = impl.find("Response export_submissions_csv") != std::string::npos;
  if (declared || defined) {
    EXPECT_NE(router.find("export_submissions_csv"), std::string::npos)
        << "export_submissions_csv dideklarasikan/definisi tapi tidak pernah "
           "di-wire ke route — dead code. Wire route-nya atau hapus handler.";
  }
}

// ===== E-d: early-return me-release koneksi ==============================

TEST(P36, Ed_SubmissionsEarlyReturnReleases) {
  auto src = read_src36("src/handlers/admin/submissions.cpp");
  ASSERT_FALSE(src.empty());
  // E-d: jalur scope-deny WAJIB pre-release sebelum return — pola lama
  // return telanjang tanpa release = koneksi sehat di-CLOSE (PQfinish oleh
  // deleter), bukan kembali ke pool (churn TCP+auth per request).
  auto w1 = between(src, "P22-M7: scope submission dicek DI handler",
                    "if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK && PQntuples(r.get())>0)");
  ASSERT_FALSE(w1.empty());
  EXPECT_NE(w1.find("real.release(c.release())"), std::string::npos)
      << "scope-deny wajib real.release(c.release()) SEBELUM return "
         "(dulu return telanjang → koneksi di-CLOSE)";
  auto w2 = between(src, "P22-M7: hapus submission hanya bila exam-nya milik actor",
                    "DELETE FROM submissions WHERE id=$1");
  ASSERT_FALSE(w2.empty());
  EXPECT_NE(w2.find("real.release(c.release())"), std::string::npos)
      << "delete: scope-deny wajib pre-release juga";
}

// ===== G5: stop tanpa backoff per-job ===================================

TEST(P36, G5_StopNoPerJobBackoff) {
  auto src = read_src36("src/queue/submission_queue.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between_last(src, "void Worker::stop(){", "\n}");
  ASSERT_FALSE(w.empty());
  EXPECT_EQ(w.find("retry_backoff_ms"), std::string::npos)
      << "final drain stop() wajib tanpa sleep backoff per-job "
         "(dulu 40 job x 5s = 200s shutdown)";
}

// ===== G6: Redis AUTH username + connect timeout =========================

TEST(P36, G6_RedisAuthUsernameAndTimeout) {
  auto src = read_src36("src/redis/redis_real.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "connect_redis", "bool redis_ping");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("redisConnectWithTimeout"), std::string::npos)
      << "koneksi Redis wajib ber-timeout (dulu redisConnect tanpa timeout — hang)";
  EXPECT_NE(w.find("AUTH %s %s"), std::string::npos)
      << "AUTH wajib kirim username+password (managed ACL user non-default — "
         "dulu AUTH %s password saja → WRONGPASS senyap)";
}

// ===== G7: BEGIN heartbeat diperiksa ====================================

TEST(P36, G7_HeartbeatBeginChecked) {
  auto src = read_src36("src/queue/submission_queue.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "static int drain_heartbeat_batch", "bool row_ok=true;");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("if(!real.exec_params(conn,\"BEGIN\",{}))"), std::string::npos)
      << "BEGIN gagal wajib di-check: requeue batch + return, jangan lanjut INSERT "
         "di koneksi rusak (dulu 500x log error)";
  EXPECT_NE(w.find("return 0"), std::string::npos)
      << "jalur BEGIN gagal wajib return 0 (batch di-requeue)";
}

// ===== G8: enqueue job pakai koneksi thread-local ========================

TEST(P36, G8_EnqueueJobThreadLocal) {
  auto src = read_src36("src/handlers/api/exams.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "static bool enqueue_job_to_redis", "// JSON escape");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("thread_local"), std::string::npos)
      << "jalur submit terpanas wajib reuse koneksi thread-local (pola queue_redis "
         "main.cpp), bukan TCP+auth Redis baru per submit";
}

// ===== G10: higienis test_pg_integration ================================

TEST(P36, G10_IntegrationTestHygiene) {
  auto src = read_src36("tests/test_pg_integration_tdd.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_NE(src.find("RaiiPgCleanup"), std::string::npos)
      << "cleanup DELETE manual setelah ASSERT wajib diganti guard RAII "
         "(ASSERT gagal mid-test → kontaminasi DB dev/CI)";
  EXPECT_NE(src.find("restore_saas_settings"), std::string::npos)
      << "mutasi saas_settings wajib snapshot + restore (jangan dipaksa permanen)";
  EXPECT_EQ(src.find("FLUSHDB"), std::string::npos)
      << "test tidak boleh FLUSHDB/DEL global queue milik proses lain";
}

// ===== F8: R2 HEAD anti-hang ============================================

TEST(P36, F8_R2HeadHasConnectTimeout) {
  auto src = read_src36("src/handlers/r2/r2.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between_last(src, "bool R2Client::verify(", "\n}");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("CURLOPT_CONNECTTIMEOUT"), std::string::npos)
      << "HEAD verify wajib connect timeout eksplisit — dulu hang hingga 15 detik "
         "menahan jalur upload PDF";
}

// ===== F11: rate-limit submit pakai X-Real-IP ============================

TEST(P36, F11_SubmitUsesRealIp) {
  auto src = read_src36("src/handlers/api/exams.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "std::string sub_ip=", "rate_limit_allowed(\\\"ratelimit:submit:\\\"");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("X-Real-IP"), std::string::npos)
      << "key rate-limit submit wajib utamakan X-Real-IP (di-set nginx dari "
         "$remote_addr) — XFF mentah bisa dipalsukan klien untuk evasi limit";
}

// ===== I5: health ready endpoint ========================================

TEST(P36, I5_HealthReadyEndpoint) {
  auto router = read_src36("src/http/router_full.cpp");
  ASSERT_FALSE(router.empty());
  EXPECT_NE(router.find("/api/health/ready"), std::string::npos)
      << "endpoint ready (probe PG+Redis) wajib terdaftar di router";
  auto src = read_src36("src/handlers/api/exams.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_NE(src.find("Response health_ready"), std::string::npos)
      << "handler health_ready wajib ada: probe PG/Redis, gagal → 503 "
         "(live = proses hidup; ready = dependency siap — jangan digabung)";
}

// ===== Dead-code: version_gate ter-wire =================================

TEST(P36, DeadCode_VersionGateWired) {
  auto router = read_src36("src/http/router_full.cpp");
  ASSERT_FALSE(router.empty());
  EXPECT_NE(router.find("middleware::version_gate"), std::string::npos)
      << "middleware version_gate ada + teruji (P5) tapi tidak pernah dipakai "
         "router — wire ke jalur API siswa atau hapus";
}
