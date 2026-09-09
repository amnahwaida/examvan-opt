// Integration test PG (TDD) — dok review pass-21 "Rekomendasi pengujian #1":
// fresh-DB TANPA skema Go → migrate() harus membuat semua tabel yang dipakai
// handler, lalu register → redeem voucher → create exam berjalan tanpa 500.
//
// Dua kelompok test:
//  A. TEMP-DB (benar-benar fresh): CREATE DATABASE sementara
//     (examvan_fresh_<pid>_<rand>) → migrate() di schema kosong → verifikasi
//     tabel + kolom → DROP DATABASE. Ini satu-satunya cara menguji "DB bersih"
//     secara jujur: handler (redeem/auth_store) memakai pool GLOBAL yang
//     terikat DATABASE_URL, sehingga trik search_path tidak mengisolasi apa pun.
//  B. SHARED-DB (DB dari DATABASE_URL, umumnya DB dev/CI): alur handler
//     end-to-end via pool global yang sama dengan produksi — register E2E
//     (register_handler + CSRF double-submit), get_setting saas_settings,
//     redeem voucher atomik (M13), create_exam ke PG store, heartbeat flusher
//     → student_access_logs. Semua baris uji dibersihkan; migrate() dipanggil
//     dulu (idempoten, IF NOT EXISTS — skema Go yang sudah lengkap tak tersentuh).
//
// SKIP otomatis (exit 0) bila DATABASE_URL tidak diset / PG tak terjangkau /
// admin DB tidak bisa dibuat — CI menjalankannya dengan service container PG.
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "config/config.hpp"
#include "db/pool.hpp"
#include "handlers/admin/exams.hpp"
#include "handlers/admin/vouchers.hpp"
#include "handlers/auth/auth_store.hpp"
#include "handlers/auth/register.hpp"
#include "http/router.hpp"
#include "queue/submission_queue.hpp"
#include "store/exam_store.hpp"
#ifdef HAS_LIBPQ
#include "db/pool_global.hpp"
#include "db/pool_real.hpp"
#include "store/exam_store_postgres.hpp"
#include <libpq-fe.h>
#endif
#ifdef HAS_HIREDIS
#include "redis/redis_real.hpp"
#include <hiredis/hiredis.h>
#endif

using namespace examvan;

// ===== infrastruktur: util SQL ===========================================

namespace {

// Env dibaca SEKALI saat static-init (sebelum gtest_main & test lain yang
// me-unsetenv DATABASE_URL/REDIS_URL, mis. test_config) — tanpa ini test
// integrasi terskip diam-diam di full-suite run.
inline const char* integ_database_url() {
  static const std::string v = [] {
    const char* e = getenv("DATABASE_URL");
    return e ? std::string(e) : std::string();
  }();
  return v.empty() ? nullptr : v.c_str();
}
inline const char* integ_redis_url() {
  static const std::string v = [] {
    const char* e = getenv("REDIS_URL");
    return e ? std::string(e) : std::string();
  }();
  return v.empty() ? nullptr : v.c_str();
}

inline std::string rnd_suffix(int n) {
  static std::mt19937 rng{std::random_device{}()};
  static const char* alnum = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string s;
  for (int i = 0; i < n; i++) s += alnum[rng() % 36];
  return s;
}

// Escape literal SQL (nilai uji dibuat sendiri, tetap di-escape demi kebiasaan).
inline std::string sq(const std::string& v) {
  std::string out;
  for (char c : v) {
    if (c == '\'') out += "''";
    else out += c;
  }
  return out;
}

#ifdef HAS_LIBPQ

// Ganti nama database di URL (untuk koneksi admin ke DB sementara).
inline std::string with_dbname(const std::string& url, const std::string& dbname) {
  auto scheme = url.find("://");
  if (scheme == std::string::npos) return "";
  auto slash = url.find('/', scheme + 3);
  if (slash == std::string::npos) return "";
  auto qm = url.find('?', slash);
  return url.substr(0, slash + 1) + dbname +
         (qm == std::string::npos ? "" : url.substr(qm));
}

inline bool exec_sql(db::RealPool& p, const std::string& sql) {
  auto c = p.acquire();
  if (!c || PQstatus(c.get()) != CONNECTION_OK) return false;
  auto r = p.exec_params(c.get(), sql, {});
  p.release(c.release());
  return r && (PQresultStatus(r.get()) == PGRES_COMMAND_OK ||
               PQresultStatus(r.get()) == PGRES_TUPLES_OK);
}

inline db::PgResultPtr query_sql(db::RealPool& p, const std::string& sql) {
  auto c = p.acquire();
  if (!c || PQstatus(c.get()) != CONNECTION_OK) return {};
  auto r = p.exec_params(c.get(), sql, {});
  p.release(c.release());
  return r;
}

inline std::string scalar(db::RealPool& p, const std::string& sql) {
  auto r = query_sql(p, sql);
  if (!r || PQresultStatus(r.get()) != PGRES_TUPLES_OK || PQntuples(r.get()) < 1 ||
      PQgetisnull(r.get(), 0, 0))
    return "";
  return PQgetvalue(r.get(), 0, 0);
}

// INSERT user langsung via SQL (kolom sama seperti INSERT auth_store.cpp) —
// register handler tidak bisa dipakai untuk menyiapkan user redeem (kuota,
// rate-limit, dst), dan jalur PG auth_store sengaja tidak dipakai di test build.
inline std::string insert_test_user(db::RealPool& p, const std::string& username) {
  (void)exec_sql(p, "DELETE FROM admin_users WHERE username='" + sq(username) + "'");
  std::string email = rnd_suffix(8) + "@t.local";
  if (!exec_sql(p, "INSERT INTO admin_users (username,name,password_hash,status,instansi,"
                   "role,email,package,operator_created) VALUES ('" +
                       sq(username) + "','U','x','active','personal','[\"guru\"]','" + email +
                       "','free',FALSE)"))
    return "";
  return scalar(p, "SELECT id FROM admin_users WHERE username='" + sq(username) + "'");
}

#endif  // HAS_LIBPQ

}  // namespace

#ifdef HAS_LIBPQ
namespace {
/* Warm-up pool global SEJAK static-init: env masih pristine (belum ada test
 * lain yang unsetenv DATABASE_URL, mis. test_config) — call_once mengunci
 * conninfo itu untuk seumur proses, sehingga drain_heartbeats_once() dan
 * handler lain tetap berfungsi di full-suite run apapun urutan testnya. */
struct GlobalPoolWarmup {
  GlobalPoolWarmup() { (void)db::global_pool(); }
};
static GlobalPoolWarmup g_pool_warmup;
}  // namespace
#endif  // HAS_LIBPQ

// ===== A. fresh-DB (database sementara) ==================================

#ifdef HAS_LIBPQ

TEST(PgIntegration, MigrateCreatesAllHandlerTablesOnFreshDatabase) {
  const char* base = integ_database_url();
  if (!base || !*base) GTEST_SKIP() << "DATABASE_URL tidak diset";

  // Koneksi admin ke DB "postgres" untuk CREATE/DROP DATABASE.
  std::string admin_url = with_dbname(base, "postgres");
  if (admin_url.empty()) GTEST_SKIP() << "DATABASE_URL tidak bisa diganti dbname-nya";
  db::RealPool admin(conninfo_from_url_or_raw(admin_url), 2);
  auto ac = admin.acquire();
  if (!ac || PQstatus(ac.get()) != CONNECTION_OK)
    GTEST_SKIP() << "koneksi admin (db postgres) tidak tersedia — skip fresh-DB";
  admin.release(ac.release());

  const std::string dbname = "examvan_fresh_" + std::to_string(getpid()) + "_" + rnd_suffix(6);
  ASSERT_TRUE(exec_sql(admin, "CREATE DATABASE " + dbname))
      << "gagal CREATE DATABASE sementara (privilege admin?)";

  bool dropped = false;
  struct DropGuard {
    db::RealPool& admin;
    const std::string& dbname;
    bool& dropped;
    ~DropGuard() {
      if (!dropped) (void)exec_sql(admin, "DROP DATABASE IF EXISTS " + dbname + " WITH (FORCE)");
    }
  } guard{admin, dbname, dropped};

  db::RealPool fresh(conninfo_from_url_or_raw(with_dbname(base, dbname)), 2);
  auto c = fresh.acquire();
  ASSERT_TRUE(c && PQstatus(c.get()) == CONNECTION_OK) << "gagal konek ke DB sementara";
  fresh.release(c.release());

  // Inti P21-T1: migrate() di DB yang benar-benar kosong.
  store::ExamStorePostgres pgstore(fresh);
  EXPECT_TRUE(pgstore.migrate()) << "migrate() harus sukses di DB kosong";
  EXPECT_TRUE(pgstore.ready());
  // Idempoten: deploy/restart kedua tetap sukses (IF NOT EXISTS).
  EXPECT_TRUE(pgstore.migrate()) << "migrate() kedua kali harus tetap sukses";

  auto r = query_sql(fresh, "SELECT table_name FROM information_schema.tables "
                            "WHERE table_schema=current_schema()");
  ASSERT_TRUE(r && PQresultStatus(r.get()) == PGRES_TUPLES_OK);
  // Selalu mulai dengan pemisah agar pencarian "|<nama>" valid utk elemen pertama.
  std::string tables = "|";
  for (int i = 0; i < PQntuples(r.get()); i++) tables += std::string(PQgetvalue(r.get(), i, 0)) + "|";
  for (const char* t : {"exams", "exam_idempotency", "submissions", "student_access_logs",
                        "exam_approvals", "admin_users", "admin_audit_logs", "saas_settings",
                        "vouchers", "voucher_redemptions", "package_settings", "exam_pengawas"}) {
    EXPECT_NE(tables.find(std::string("|") + t), std::string::npos)
        << "tabel " << t << " tidak dibuat oleh migrate()";
  }

  // admin_users versi migrate wajib memadai untuk INSERT register (auth_store).
  auto cols = query_sql(fresh, "SELECT column_name FROM information_schema.columns "
                               "WHERE table_name='admin_users'");
  ASSERT_TRUE(cols && PQresultStatus(cols.get()) == PGRES_TUPLES_OK);
  std::string col = "|";
  for (int i = 0; i < PQntuples(cols.get()); i++) col += std::string(PQgetvalue(cols.get(), i, 0)) + "|";
  for (const char* cn : {"name", "email", "password_hash", "role", "instansi", "status",
                         "otp_code", "otp_expiry", "otp_attempts", "package", "max_exams",
                         "max_pdf_size", "max_concurrent_exams", "max_storage_size",
                         "expires_at", "registered_ip", "operator_created", "whatsapp_number",
                         "base_role", "package_role"}) {
    EXPECT_NE(col.find(std::string("|") + cn), std::string::npos)
        << "kolom admin_users." << cn << " hilang — INSERT register akan gagal di DB bersih";
  }

  dropped = exec_sql(admin, "DROP DATABASE " + dbname + " WITH (FORCE)");
  ASSERT_TRUE(dropped) << "drop DB sementara gagal";
}

// Kontrak register-produksi: ekstrak PERSIS SQL INSERT dari auth_store.cpp
// (sumber kebenaran produksi) dan jalankan di DB sementara — skema hasil
// migrate() harus memadai untuk INSERT register apa adanya, bukan tiruan yang
// bisa usang. Gagal di sini = register produksi gagal di fresh-DB (P21-T1).
TEST(PgIntegration, FreshDbSchemaFitsProductionRegisterInsert) {
  const char* base = integ_database_url();
  if (!base || !*base) GTEST_SKIP() << "DATABASE_URL tidak diset";

  std::string admin_url = with_dbname(base, "postgres");
  if (admin_url.empty()) GTEST_SKIP() << "DATABASE_URL tidak bisa diganti dbname-nya";
  db::RealPool admin(conninfo_from_url_or_raw(admin_url), 2);
  auto ac = admin.acquire();
  if (!ac || PQstatus(ac.get()) != CONNECTION_OK) GTEST_SKIP() << "koneksi admin tidak tersedia";
  admin.release(ac.release());

  std::ifstream auth("src/handlers/auth/auth_store.cpp");
  std::string auth_src((std::istreambuf_iterator<char>(auth)),
                       std::istreambuf_iterator<char>());
  ASSERT_FALSE(auth_src.empty());

  // Ekstrak literal C++ multi-baris yang memuat "INSERT INTO admin_users":
  // gabungkan literal-literal berurutan (tersembunyi di sumber sbg
  // "..." "..." dengan escape \" di dalamnya).
  auto key = auth_src.find("INSERT INTO admin_users");
  ASSERT_NE(key, std::string::npos);
  auto open = auth_src.rfind('"', key);
  ASSERT_NE(open, std::string::npos);
  std::string sql;
  bool in_str = true;
  size_t i = open + 1;
  for (; i < auth_src.size(); ++i) {
    char ch = auth_src[i];
    if (!in_str) {
      if (isspace(static_cast<unsigned char>(ch))) continue;
      if (ch == '"') {
        in_str = true;
        continue;
      }
      break;  // akhir pernyataan (koma/`;`/kurung) — berhenti
    }
    if (ch == '\\') {
      if (i + 1 < auth_src.size()) {
        char nx = auth_src[i + 1];
        if (nx == '"') sql += '"';
        else if (nx == '\\') sql += '\\';
        else if (nx == 'n') sql += '\n';
        else sql += nx;
        ++i;
      }
      continue;
    }
    if (ch == '"') {
      in_str = false;
      continue;
    }
    sql += ch;
  }
  ASSERT_FALSE(in_str) << "ekstraksi literal gagal (kutip tak seimbang)";
  ASSERT_NE(sql.find("INSERT INTO admin_users"), std::string::npos);
  ASSERT_NE(sql.find("make_interval"), std::string::npos)
      << "SQL yang diekstrak bukan INSERT register yang utuh";

  const std::string dbname = "examvan_fresh_ins_" + std::to_string(getpid()) + "_" + rnd_suffix(6);
  ASSERT_TRUE(exec_sql(admin, "CREATE DATABASE " + dbname));
  bool dropped = false;
  struct DropGuard {
    db::RealPool& admin;
    const std::string& dbname;
    bool& dropped;
    ~DropGuard() {
      if (!dropped) (void)exec_sql(admin, "DROP DATABASE IF EXISTS " + dbname + " WITH (FORCE)");
    }
  } guard{admin, dbname, dropped};

  db::RealPool fresh(conninfo_from_url_or_raw(with_dbname(base, dbname)), 2);
  store::ExamStorePostgres pgstore(fresh);
  ASSERT_TRUE(pgstore.migrate()) << "migrate() harus sukses di DB kosong";

  // Substitusi parameter $12..$1 (urut turun agar $1 tidak merusak $11/$12).
  // Param teks wajib dikutip; numerik dibiarkan telanjang (posisi bertipe).
  const std::string username = "regprod_" + rnd_suffix(8);
  const std::vector<std::tuple<std::string, std::string, bool>> params = {
      {"$12", "10.99.9.9", true},              // registered_ip
      {"$11", "0", false},                     // otp_expiry_epoch (0 → NULL)
      {"$10", "", true},                       // otp_code
      {"$9", "14", false},                     // active_days
      {"$8", rnd_suffix(8) + "@t.local", true},  // email
      {"$7", "52428800", false},               // max_storage_size
      {"$6", "2", false},                      // max_concurrent_exams
      {"$5", "1048576", false},                // max_pdf_size
      {"$4", "3", false},                      // max_exams
      {"$3", "active", true},                  // status
      {"$2", "bcrypt-hash-fake", true},        // password_hash
      {"$1", username, true},                  // username
  };
  for (const auto& p : params) {
    const auto& ph = std::get<0>(p);
    const auto& val = std::get<1>(p);
    bool is_text = std::get<2>(p);
    // Ganti SEMUA kemunculan placeholder (mis. $11 muncul 2× di CASE WHEN).
    const std::string rep = is_text ? "'" + val + "'" : val;
    for (auto at = sql.find(ph); at != std::string::npos; at = sql.find(ph, at + rep.size())) {
      sql.replace(at, ph.size(), rep);
    }
  }

  {
    auto ic = fresh.acquire();
    ASSERT_TRUE(ic && PQstatus(ic.get()) == CONNECTION_OK);
    auto ir = fresh.exec_params(ic.get(), sql, {});
    EXPECT_TRUE(ir && (PQresultStatus(ir.get()) == PGRES_COMMAND_OK ||
                       PQresultStatus(ir.get()) == PGRES_TUPLES_OK))
        << "INSERT register versi PRODUKSI gagal di skema hasil migrate() — "
           "register akan 500 di DB bersih (P21-T1): "
        << (ir ? PQresultErrorMessage(ir.get()) : "no result") << " | SQL: " << sql;
    fresh.release(ic.release());
  }

  dropped = exec_sql(admin, "DROP DATABASE " + dbname + " WITH (FORCE)");
  ASSERT_TRUE(dropped);
}

#endif  // HAS_LIBPQ

// ===== B. shared-DB: alur handler end-to-end =============================

#ifdef HAS_LIBPQ

TEST(PgIntegration, SharedDbMigrateIsIdempotentAndReady) {
  const char* u = integ_database_url();
  if (!u || !*u) GTEST_SKIP() << "DATABASE_URL tidak diset";
  db::RealPool pool(conninfo_from_url_or_raw(u), 4);
  auto c = pool.acquire();
  if (!c || PQstatus(c.get()) != CONNECTION_OK)
    GTEST_SKIP() << "PG tidak terjangkau — skip integrasi";
  pool.release(c.release());
  store::ExamStorePostgres pgstore(pool);
  EXPECT_TRUE(pgstore.migrate());
  EXPECT_TRUE(pgstore.ready());
}

TEST(PgIntegration, SourceContractAuthStoreReadsSaasSettings) {
  // Build test (EXAMVAN_TESTING) memakai mode memori untuk auth_store, jadi
  // kontrak produksi dikunci dari sumber: get_setting wajib membaca tabel
  // saas_settings (P21-T1) — halaman register/recovery bergantung padanya.
  std::ifstream f("src/handlers/auth/auth_store.cpp");
  std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  ASSERT_FALSE(src.empty()) << "src/handlers/auth/auth_store.cpp harus ada";
  EXPECT_NE(src.find("FROM saas_settings"), std::string::npos)
      << "get_setting produksi wajib membaca saas_settings";
}

TEST(PgIntegration, RegisterHandlerWritesUserStore) {
  const char* u = integ_database_url();
  if (!u || !*u) GTEST_SKIP() << "DATABASE_URL tidak diset";
  db::RealPool pool(conninfo_from_url_or_raw(u), 4);
  auto c = pool.acquire();
  if (!c || PQstatus(c.get()) != CONNECTION_OK) GTEST_SKIP() << "PG tidak terjangkau";
  pool.release(c.release());

  // Deterministik: matikan verifikasi email + turnstile via saas_settings
  // (build produksi membaca ini dari PG; sekalian membuktikan tabel bisa
  // ditulis di DB target).
  ASSERT_TRUE(exec_sql(pool, "INSERT INTO saas_settings (key,value) VALUES "
                             "('email_verification_enabled','0') ON CONFLICT (key) DO UPDATE "
                             "SET value='0'"));
  ASSERT_TRUE(exec_sql(pool, "INSERT INTO saas_settings (key,value) VALUES "
                             "('turnstile_enabled','0') ON CONFLICT (key) DO UPDATE SET "
                             "value='0'"));
  handlers::auth::reset_register_limit_for_test();

  // CSRF double-submit: token tersedia karena csrf_ok membaca cookie + form —
  // tidak perlu seed memori; token acak cukup (cookie == body).
  const std::string tok = "integcsrf-" + rnd_suffix(12);
  const std::string username = "integreg_" + rnd_suffix(8);

  Config cfg;
  Request req;
  req.method = "POST";
  req.headers["Cookie"] = "csrf_token=" + tok;
  req.headers["Content-Type"] = "application/x-www-form-urlencoded";
  req.headers["X-Requested-With"] = "XMLHttpRequest";
  req.headers["X-Real-IP"] = "10.99.0.99";
  req.body = "username=" + username + "&email=integ_" + rnd_suffix(6) +
             "@t.local&password=p4ssw0rd!x&password_confirm=p4ssw0rd!x&csrf_token=" + tok;

  auto res = handlers::auth::register_handler(req, cfg);
  ASSERT_EQ(res.status, 200) << "register gagal: " << res.body;
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos) << res.body;

  // Build test: INSERT auth_store menulis ke lapisan memori (produksi: PG —
  // kontrak skema INSERT-produksi dikunci test fresh-DB di bawah). Yang diuji
  // di sini: CSRF diterima, validasi lolos, user tersimpan dengan status
  // aktif + kuota default.
  handlers::auth::RegisteredUser stored;
  ASSERT_TRUE(handlers::auth::find_registered_user(username, stored))
      << "user register harus tersimpan di user store";
  EXPECT_EQ(stored.status, "active");
  EXPECT_EQ(stored.max_exams, 3);

  handlers::auth::delete_registered_user(username);
  handlers::auth::clear_registered_users_for_test();
}

TEST(PgIntegration, VoucherRedeemEndToEndViaHandler) {
  const char* u = integ_database_url();
  if (!u || !*u) GTEST_SKIP() << "DATABASE_URL tidak diset";
  db::RealPool pool(conninfo_from_url_or_raw(u), 4);
  auto c = pool.acquire();
  if (!c || PQstatus(c.get()) != CONNECTION_OK) GTEST_SKIP() << "PG tidak terjangkau";
  pool.release(c.release());

  const std::string username = "integredeem_" + rnd_suffix(8);
  std::string uid = insert_test_user(pool, username);
  ASSERT_FALSE(uid.empty()) << "gagal INSERT admin_users untuk test";
  const std::string code = "INTEG" + rnd_suffix(6);

  // Paket + voucher aktif max_usage=1 (seed langsung, pola halaman admin).
  ASSERT_TRUE(exec_sql(pool,
                       "INSERT INTO package_settings (pkg_key,label,max_exams,max_pdf_size,"
                       "max_concurrent_exams,max_storage_size,max_users,role) VALUES "
                       "('integgratis','Paket Integ',5,5242880,1,52428800,1,'[\"guru\"]') ON "
                       "CONFLICT (pkg_key) DO UPDATE SET max_exams=5"));
  ASSERT_TRUE(exec_sql(pool, "INSERT INTO vouchers (code,package,duration_type,max_usage,"
                             "used_count,is_active) VALUES ('" +
                                 sq(code) + "','integgratis','bulanan',1,0,TRUE)"));

  Request req;
  req.method = "POST";
  req.headers["X-Internal-Admin-Id"] = uid;
  req.headers["Content-Type"] = "application/x-www-form-urlencoded";
  req.body = "code=" + code;

  auto res = handlers::admin::redeem_voucher(req);
  ASSERT_EQ(res.status, 200) << "redeem gagal di PG: " << res.body;
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos) << res.body;

  // used_count terpakai.
  EXPECT_EQ(scalar(pool, "SELECT used_count FROM vouchers WHERE code='" + sq(code) + "'"), "1");
  // Redemption row + entitlement user dalam SATU transaksi (M13).
  EXPECT_EQ(scalar(pool, "SELECT count(*) FROM voucher_redemptions r JOIN admin_users u ON "
                         "u.id=r.user_id WHERE u.username='" +
                             sq(username) + "'"),
            "1");
  EXPECT_EQ(scalar(pool, "SELECT package FROM admin_users WHERE username='" + sq(username) + "'"),
            "integgratis");
  EXPECT_EQ(scalar(pool, "SELECT max_exams FROM admin_users WHERE username='" + sq(username) + "'"),
            "5");

  // Redeem kedua untuk voucher max_usage=1 harus ditolak (anti double-spend).
  auto res2 = handlers::admin::redeem_voucher(req);
  EXPECT_EQ(res2.status, 400) << "redeem kedua voucher max_usage=1 harus 400: " << res2.body;

  (void)exec_sql(pool, "DELETE FROM voucher_redemptions WHERE user_id=" + uid);
  (void)exec_sql(pool, "DELETE FROM vouchers WHERE code='" + sq(code) + "'");
  (void)exec_sql(pool, "DELETE FROM package_settings WHERE pkg_key='integgratis'");
  (void)exec_sql(pool, "DELETE FROM admin_users WHERE username='" + sq(username) + "'");
}

TEST(PgIntegration, CreateExamPersistsToPostgresStore) {
  const char* u = integ_database_url();
  if (!u || !*u) GTEST_SKIP() << "DATABASE_URL tidak diset";
  db::RealPool pool(conninfo_from_url_or_raw(u), 4);
  auto c = pool.acquire();
  if (!c || PQstatus(c.get()) != CONNECTION_OK) GTEST_SKIP() << "PG tidak terjangkau";
  pool.release(c.release());

  // Swap store aktif ke PG (RAII restore ke store default test build).
  store::ExamStorePostgres pgstore(pool);
  ASSERT_TRUE(pgstore.migrate());
  store::set_active_store(&pgstore);
  struct Restore {
    ~Restore() { store::set_active_store(nullptr); }
  } restore;

  // R2 mock: create_exam wajib upload PDF; mock mencegah panggilan jaringan.
  std::vector<std::pair<std::string, std::string>> uploads;
  handlers::admin::set_upload_mock_for_test(
      [&](const std::string& k, const std::string& d) { uploads.emplace_back(k, d); });
  struct ClearMock {
    ~ClearMock() { handlers::admin::set_upload_mock_for_test(nullptr); }
  } clear_mock;

  Request req;
  req.method = "POST";
  req.headers["X-Internal-Admin-Id"] = "1";
  req.headers["Content-Type"] = "application/x-www-form-urlencoded";
  req.headers["Idempotency-Key"] = "integ-idem-" + rnd_suffix(10);
  req.body = "name=Ujian+Integ+PG&file_path=integ-test.pdf&size_bytes=100";

  auto r1 = handlers::admin::create_exam(req);
  ASSERT_EQ(r1.status, 201) << "create_exam harus sukses di PG store: " << r1.body;

  // Exam tersimpan di PG (bukan hanya memori).
  EXPECT_EQ(scalar(pool, "SELECT count(*) FROM exams WHERE name='Ujian Integ PG'"), "1")
      << "exam harus tersimpan di tabel exams PG";

  // Replay idempotency: response identik, tidak menambah baris.
  auto r2 = handlers::admin::create_exam(req);
  EXPECT_EQ(r2.status, 201);
  EXPECT_EQ(r1.body, r2.body) << "replay idempotency harus mengembalikan body yang sama";
  EXPECT_EQ(scalar(pool, "SELECT count(*) FROM exams WHERE name='Ujian Integ PG'"), "1");

  (void)exec_sql(pool, "DELETE FROM exam_idempotency WHERE idempotency_key='" +
                           sq(req.headers["Idempotency-Key"]) + "'");
  (void)exec_sql(pool, "DELETE FROM exams WHERE name='Ujian Integ PG'");
}

#ifdef HAS_HIREDIS

TEST(PgIntegration, HeartbeatFlusherWritesAccessLogsAndPlaceholder) {
  const char* u = integ_database_url();
  if (!u || !*u) GTEST_SKIP() << "DATABASE_URL tidak diset";
  const char* ru = integ_redis_url();
  if (!ru || !*ru) GTEST_SKIP() << "REDIS_URL tidak diset";
  auto ctx = redis_real::connect_redis(ru);
  if (!ctx) GTEST_SKIP() << "Redis tidak terjangkau — skip E2E heartbeat";

  db::RealPool pool(conninfo_from_url_or_raw(u), 4);
  auto c = pool.acquire();
  if (!c || PQstatus(c.get()) != CONNECTION_OK) GTEST_SKIP() << "PG tidak terjangkau";
  pool.release(c.release());
  ASSERT_TRUE(exec_sql(pool, "SELECT 1"));

  // Exam penerima heartbeat (FK student_access_logs/submissions → exams).
  std::string eid = scalar(pool, "INSERT INTO exams (name,file_path,size_bytes,token,"
                                 "active_token,status,security_level,strict_mode,public_results,"
                                 "show_answers,created_by,created_at) VALUES "
                                 "('Ujian Integ HB','integ-hb.pdf',1,'tokinteg-" +
                                     rnd_suffix(8) + "','','active','standard',0,0,0,1,now()) "
                                 "RETURNING id");
  ASSERT_FALSE(eid.empty()) << "gagal INSERT exam untuk test heartbeat";

  // Payload heartbeat ke antrean Redis (format sama dengan parser queue).
  const std::string payload =
      "{\"exam_id\":\"" + eid + "\",\"mac_address\":\"AA:INTEG:00:01\",\"student_name\":\"Integ "
      "HB\",\"exam_number\":\"007\",\"student_class\":\"XI-A\",\"device_info\":\"utest\","
      "\"ip_address\":\"10.1.1.9\",\"event\":\"heartbeat\",\"last_seen\":\"\"}";
  auto* push = reinterpret_cast<redisReply*>(redisCommand(ctx.get(), "LPUSH %s %b",
                                                          queue::kHeartbeatQueueKey,
                                                          payload.data(), payload.size()));
  ASSERT_TRUE(push) << "LPUSH antrean heartbeat gagal";
  freeReplyObject(push);

  // Flusher (jalur produksi — kini pakai pool global P24).
  int flushed = queue::drain_heartbeats_once();
  EXPECT_GE(flushed, 1) << "heartbeat tidak ter-flush";

  // Log akses + placeholder submission muncul (paritas Go).
  EXPECT_EQ(scalar(pool, "SELECT count(*) FROM student_access_logs WHERE exam_id=" + eid +
                             " AND student_identifier='AA:INTEG:00:01'"),
            "1")
      << "heartbeat harus masuk student_access_logs";
  EXPECT_EQ(scalar(pool, "SELECT count(*) FROM submissions WHERE exam_id=" + eid +
                             " AND mac_address='AA:INTEG:00:01'"),
            "1")
      << "placeholder submission harus dibuat dari heartbeat";

  (void)exec_sql(pool, "DELETE FROM student_access_logs WHERE exam_id=" + eid);
  (void)exec_sql(pool, "DELETE FROM submissions WHERE exam_id=" + eid);
  (void)exec_sql(pool, "DELETE FROM exams WHERE id=" + eid);
  auto* del =
      reinterpret_cast<redisReply*>(redisCommand(ctx.get(), "DEL %s", queue::kHeartbeatQueueKey));
  if (del) freeReplyObject(del);
}

#endif  // HAS_HIREDIS

#endif  // HAS_LIBPQ

// ===== non-PG fallback: kontrak source tetap dikunci =====================

TEST(PgIntegration, SourceContractVoucherRedeemTransactional) {
  // Tanpa PG test harus tetap mengunci kontrak M13: redeem pakai
  // BEGIN + SELECT FOR UPDATE + COMMIT (anti double-spend).
  std::ifstream f("src/handlers/admin/vouchers.cpp");
  std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto pos = src.find("Response redeem_voucher");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 4000);
  EXPECT_NE(seg.find("BEGIN"), std::string::npos);
  EXPECT_NE(seg.find("FOR UPDATE"), std::string::npos);
  EXPECT_NE(seg.find("COMMIT"), std::string::npos);
}
