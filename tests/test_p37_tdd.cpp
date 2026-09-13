// P37 — kontrak TDD remediasi prioritas review pass-25 (2026-09-10).
// Temuan yang dikunci test ini (doc review-temuan-2026-09-10-pass25.md):
//  HIGH:
//   1. F12 — webhook OTP: release-before-use (UPDATE dieksekusi di koneksi
//      null) + hardening kelas: exec_params tolak koneksi-null.
//   2. I11 — .dockerignore mengecualikan path yang di-COPY Dockerfile
//      → docker compose build gagal.
//   3. D7 — bulk-toggle/bulk-delete lintas instansi (JOIN owner tidak
//      membandingkan instansi pemilik vs aktor).
//   4. E-l — instansi_update: UPDATE sukses lalu dibalas 503 selamanya.
//   5. E-m — delete_voucher tanpa gate 503 → fake-200 saat PG down.
//  MEDIUM prioritas:
//   6. F14 — request_approval: PG down → 200 "approved" tanpa persist.
//   7. E-d+G18 — release-discipline: early-return tanpa real.release() di
//      submissions.cpp & system_apps_page (koneksi di-close, bukan ke pool).
//   8. G14 — job hilang tanpa jejak saat Redis mati (spool file lokal).
//   9. G15 — Redis: connect timeout + reconnect + AUTH user/pass + SELECT
//      dicek + try_acquire_job bedakan error vs unlocked.
//  10. G17 — /api/health hardcoded "db":"ok" → probe dependency nyata.
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>

namespace {

std::string read_src37(const std::string& p) {
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

int count_of(const std::string& hay, const std::string& needle) {
  int n = 0;
  size_t p = 0;
  while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
  return n;
}

}  // namespace

// ===== F12: webhook OTP release-before-use ================================

TEST(P37, F12_WebhookNoReleaseBeforeUpdate) {
  auto src = read_src37("src/handlers/api/webhook.cpp");
  ASSERT_FALSE(src.empty());
  // Cabang baca row → UPDATE: TIDAK ada release (koneksi masih dipakai).
  // (Dulu: real.release prematur di sini → UPDATE dieksekusi di koneksi null.)
  auto w1 = between(src, "std::string reg_phone=",
                    "UPDATE admin_users SET status='active'");
  ASSERT_FALSE(w1.empty());
  EXPECT_EQ(count_of(w1, "real.release"), 0)
      << "koneksi TIDAK boleh di-release antara SELECT per-user dan UPDATE "
         "aktivasi (F12: exec di koneksi null → aktivasi 100% gagal senyap)";
  // Cabang UPDATE: TEPAT SATU release (dipakai bersama kedua exit sukses/
  // gagal — exit setelah release, bukan sebelum).
  auto w2 = between(src, "UPDATE admin_users SET status='active'",
                    "Gagal mengaktifkan akun");
  ASSERT_FALSE(w2.empty());
  EXPECT_EQ(count_of(w2, "real.release"), 1)
      << "cabang UPDATE wajib satu release tunggal setelah UPDATE — "
         "kedua exit (sukses & gagal) memakainya; dulu exec di koneksi null";
}

TEST(P37, F12_ExecParamsRejectsNullConn) {
  auto src = read_src37("src/db/pool_real.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "PgResultPtr RealPool::exec_params(PGconn* c",
                   "PgResultPtr RealPool::exec_params_nullable");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("if(!c)"), std::string::npos)
      << "exec_params wajib menolak koneksi null (PQexecParams(nullptr) "
         "silent-fail — kelas F12)";
  auto w2 = between(src, "PgResultPtr RealPool::exec_params_nullable(PGconn* c",
                    "PgResultPtr RealPool::exec_params_pooled");
  ASSERT_FALSE(w2.empty());
  EXPECT_NE(w2.find("if(!c)"), std::string::npos)
      << "exec_params_nullable wajib menolak koneksi null juga";
}

// ===== I11: .dockerignore vs Dockerfile COPY ===============================

TEST(P37, I11_DockerignoreMustNotExcludeCopiedSources) {
  auto dockerfile = read_src37("Dockerfile");
  auto ignore = read_src37(".dockerignore");
  ASSERT_FALSE(dockerfile.empty());
  ASSERT_FALSE(ignore.empty());

  std::set<std::string> ignored;
  {
    std::istringstream is(ignore);
    std::string line;
    while (std::getline(is, line)) {
      auto h = line.find_first_not_of(" \t");
      if (h == std::string::npos || line[h] == '#') continue;
      std::istringstream ls(line);
      std::string tok;
      while (ls >> tok) if (tok != "!") ignored.insert(tok);
    }
  }
  auto is_ignored = [&](const std::string& path) {
    if (ignored.count(path)) return true;
    for (const auto& pat : ignored) {
      if (pat.size() > 1 && pat[0] == '*' &&
          path.size() >= pat.size() - 1 &&
          path.compare(path.size() - (pat.size() - 1), pat.size() - 1,
                       pat.substr(1)) == 0) return true;
    }
    return false;
  };

  std::istringstream df(dockerfile);
  std::string line;
  while (std::getline(df, line)) {
    if (line.rfind("COPY ", 0) != 0) continue;
    std::istringstream ls(line);
    std::string cmd, tok;
    ls >> cmd;
    std::vector<std::string> toks;
    while (ls >> tok) toks.push_back(tok);
    if (toks.size() < 2) continue;
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
      const std::string& s = toks[i];
      if (s == "." || s == "./") continue;  // build context root
      EXPECT_FALSE(is_ignored(s))
          << "I11: '" << s << "' di-COPY Dockerfile tapi di-exclude "
             ".dockerignore → docker compose build GAGAL";
    }
  }
}

// ===== D7: scope bulk lintas instansi ======================================

TEST(P37, D7_BulkScopeComparesOwnerInstansi) {
  auto src = read_src37("src/handlers/admin/exams.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "static bool exam_bulk_scope_ok", "Response bulk_toggle_exams");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("me.instansi=owner.instansi"), std::string::npos)
      << "JOIN owner wajib membandingkan instansi PEMILIK ujian dengan "
         "instansi aktor — dulu operator instansi A bisa bulk-delete ujian "
         "instansi B (D7, bypass multi-tenancy)";
  // Bentuk lama: SQL berakhir 'personal'" langsung diikuti params — tanpa
  // perbandingan owner.instansi apa pun.
  EXPECT_EQ(w.find("me.instansi<>'personal'\\\","), std::string::npos)
      << "bentuk lama (JOIN owner tanpa cek instansi pemilik) wajib dihapus";
}

// ===== E-l: instansi_update sentinel =======================================

TEST(P37, El_InstansiUpdateUsesResultSentinel) {
  auto src = read_src37("src/handlers/admin/users.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "Response instansi_update(const Request& req){",
                   "} // namespace examvan::handlers::admin");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("result==\"ok\""), std::string::npos)
      << "instansi_update wajib pakai sentinel hasil (pola change_password): "
         "UPDATE sukses → 200; PG gagal → 503 (dulu selalu 503 walau UPDATE "
         "sudah commit — fitur 100% rusak, E-l)";
}

// ===== E-m: delete_voucher fail-closed =====================================

TEST(P37, Em_DeleteVoucherFailClosed) {
  auto src = read_src37("src/handlers/admin/vouchers.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "Response delete_voucher(const Request& req){",
                   "// ===== redemptions per voucher");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("pg_configured_from_env()"), std::string::npos)
      << "delete_voucher wajib gate 503 saat PG dikonfigurasi tapi tidak "
         "terjangkau — dulu fake-200 \"Voucher dihapus\" padahal baris tetap "
         "ada & redeemable (E-m)";
}

// ===== F14: request_approval fail-closed ===================================

TEST(P37, F14_RequestApprovalFailClosedOnPgDown) {
  auto src = read_src37("src/handlers/api/exams.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "Response request_approval(const Request& req){",
                   "Response exam_by_token(const Request& req){");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("pg_used"), std::string::npos)
      << "request_approval wajib menandai pg_used (idiom persist_submission_"
         "pending): PG down → 503, bukan 200 \"approved\" tanpa baris "
         "approval (F14)";
}

// ===== E-d + G18: release-discipline early-return ==========================

TEST(P37, Ed_SubmissionsEveryPathReleases) {
  auto src = read_src37("src/handlers/admin/submissions.cpp");
  ASSERT_FALSE(src.empty());
  auto w1 = between(src, "Response submission_detail(const Request& req){",
                    "Response queue_status(const Request& req){");
  ASSERT_FALSE(w1.empty());
  EXPECT_GE(count_of(w1, "real.release("), 2)
      << "submission_detail: pre-release jalur scope-deny + final release "
         "(dulu scope-deny return telanjang → koneksi sehat di-CLOSE, E-d)";
  // delete_submission adalah handler terakhir — window sampai EOF.
  auto w2 = between(src, "Response delete_submission(const Request& req){",
                    "} // namespace examvan::handlers::admin");
  ASSERT_FALSE(w2.empty());
  EXPECT_GE(count_of(w2, "real.release("), 2)
      << "delete_submission: pre-release scope-deny + final release (E-d)";
}

TEST(P37, G18_SystemAppsPageEveryPathReleases) {
  auto src = read_src37("src/handlers/admin/settings.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "Response system_apps_page(const Request& req){",
                   "if(req.method==\"POST\") { Response r; r.status=501;");
  ASSERT_FALSE(w.empty());
  EXPECT_GE(count_of(w, "real.release("), 3)
      << "system_apps_page: 2 pre-release (jalur DELETE/POST error) + 1 final "
         "release — dulu SEMUA return 400/404/500/502/201/200 menutup koneksi "
         "(deleter=PQfinish), churn TCP+auth PG per request admin (G18)";
}

// ===== G14: durabilitas push_failed ========================================

TEST(P37, G14_PushFailedSpoolsLocallyWhenRedisDown) {
  auto src = read_src37("src/queue/submission_queue.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "void SubmissionQueue::push_failed(const SubmissionJob& job) const{",
                   "void SubmissionQueue::store_result");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("append_spool_line"), std::string::npos)
      << "push_failed wajib spool jejak lokal (JSONL append) — Redis mati "
         "penuh berarti fallback menulis ke Redis yang sama yang mati: job "
         "hilang tanpa jejak, jawaban siswa 'belum submit' (G14)";
  EXPECT_NE(src.find("kSpoolPath"), std::string::npos)
      << "konstanta path spool lokal wajib ada";
  // Worker::stop final-drain dulu sleep backoff per-job (G5 re-report pass-25)
  auto ws = between(src, "void Worker::stop(){", "\n}");
  ASSERT_FALSE(ws.empty());
  EXPECT_EQ(ws.find("retry_backoff_ms"), std::string::npos)
      << "final drain stop() tanpa sleep per-job (dulu 40x5s = shutdown 200s)";
}

// ===== G15: hiredis hardening ==============================================

TEST(P37, G15_RedisConnectTimeoutReconnectAuthUser) {
  auto src = read_src37("src/redis/redis_real.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "RedisPtr connect_redis(const std::string& url){",
                   "bool redis_ping(");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("redisConnectWithTimeout"), std::string::npos)
      << "koneksi Redis wajib ber-timeout (dulu redisConnect tanpa timeout: "
         "BLACKHOLE = thread menggantung tanpa batas, G15)";
  EXPECT_NE(w.find("AUTH %s %s"), std::string::npos)
      << "AUTH wajib username+password (Redis 6+ ACL user non-default — "
         "dulu AUTH password saja → WRONGPASS senyap)";
  EXPECT_NE(w.find("redis_reset"), std::string::npos)
      << "wrapper reconnect (redis_reset) wajib ada — dulu Redis restart sekali "
         "= worker BRPOP mati permanen seumur proses (G15)";
  auto sel = between(w, "SELECT %d", "return ptr;");
  ASSERT_FALSE(sel.empty());
  EXPECT_NE(sel.find("if(!r)"), std::string::npos)
      << "hasil SELECT db wajib dicek — dulu redis://host/db diam-diam "
         "menulis ke db 0";
}

TEST(P37, G15_TryAcquireJobDistinguishesErrorFromUnlocked) {
  auto src = read_src37("src/redis/client.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "bool RedisClient::try_acquire_job(const std::string& job, int ttl){",
                   "void RedisClient::release_job");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("redis_exists"), std::string::npos)
      << "try_acquire_job wajib bedakan error Redis vs unlocked (probe EXISTS): "
         "error → return false (fail-closed), BUKAN fallback in-process yang "
         "bikin dua replika double-run job yang sama (G15)";
}

// ===== G17: /api/health probe nyata ========================================

TEST(P37, G17_HealthProbesDependencies) {
  auto src = read_src37("src/server/server.cpp");
  ASSERT_FALSE(src.empty());
  auto w = between(src, "std::string health_json(const Config& cfg) {",
                   "} // namespace examvan::server");
  ASSERT_FALSE(w.empty());
  EXPECT_NE(w.find("global_pool"), std::string::npos)
      << "health wajib probe PG nyata (global_pool()->ping()) — dulu "
         "hardcoded \"db\":\"ok\" walau DB mati (G17)";
  EXPECT_NE(w.find("redis_ping"), std::string::npos)
      << "health wajib PING Redis nyata (bukan flag scheme)";
  EXPECT_NE(w.find("LLEN"), std::string::npos)
      << "health wajib lapor kedalaman antrean via LLEN (bukan 0 literal)";
}
