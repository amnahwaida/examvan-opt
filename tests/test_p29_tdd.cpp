// Pass-21 T1-T9 (TDD kontrak sumber): remediasi temuan review pass-21.
//
// T1 (HIGH): fresh-DB bootstrap gap — migrate() wajib membuat SEMUA tabel yang
//   dipakai handler: saas_settings, vouchers, voucher_redemptions,
//   package_settings, exam_pengawas; plus ALTER admin_users ADD COLUMN untuk
//   kolom yang dipakai INSERT register (auth_store.cpp) di DB bersih.
// T2 (HIGH): koneksi PG baru per-request — larangan pola `with_pg` yang
//   membangun RealPool stack-lokal per panggilan (koneksi di-PQfinish saat
//   scope keluar). Wajib satu pool proses-wide via examvan::db::global_pool().
// T3: protobuf-helper CDN fallback menunjuk bundle lokal yang tidak ada —
//   hapus branch CDN (dead code; helper tidak di-include template mana pun).
// T4: versi "2.7.2" hardcode 12 titik — wajib satu sumber Config::version.
// T5: queue_status membaca examvan:submissions:failed yang tidak pernah
//   ditulis worker — job gagal retry-habis wajib LPUSH ke failed queue.
// T7: dashboard_page append X-User header ke HTML — hapus (debug leftover).
// T9: voucher expires_at tanpa validasi format — wajib validasi → 400.
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_src29(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ===== T1: schema fresh-DB ==============================================

TEST(P29, MigrateCreatesSaasSettingsTable){
  auto src=read_src29("src/store/exam_store_postgres.cpp");
  EXPECT_NE(src.find("CREATE TABLE IF NOT EXISTS saas_settings"), std::string::npos)
    << "fresh-DB: saas_settings dipakai settings/recovery/register/auth_store — migrate() wajib membuatnya";
}

TEST(P29, MigrateCreatesVoucherTables){
  auto src=read_src29("src/store/exam_store_postgres.cpp");
  EXPECT_NE(src.find("CREATE TABLE IF NOT EXISTS vouchers"), std::string::npos)
    << "fresh-DB: vouchers dipakai seluruh alur voucher";
  EXPECT_NE(src.find("CREATE TABLE IF NOT EXISTS voucher_redemptions"), std::string::npos)
    << "fresh-DB: voucher_redemptions dipakai redeem/activate/mine";
  EXPECT_NE(src.find("CREATE TABLE IF NOT EXISTS package_settings"), std::string::npos)
    << "fresh-DB: package_settings dipakai entitlement redeem";
}

TEST(P29, MigrateCreatesExamPengawasTable){
  auto src=read_src29("src/store/exam_store_postgres.cpp");
  EXPECT_NE(src.find("CREATE TABLE IF NOT EXISTS exam_pengawas"), std::string::npos)
    << "fresh-DB: exam_pengawas dipakai scoping pengawas/delegate/save_questions";
}

TEST(P29, MigrateExtendsAdminUsersColumns){
  // Kolom yang dipakai INSERT register (auth_store.cpp:255-262) + edit_user.
  // Kontrak: daftar literal col_type berisi tiap "<col> <TYPE>..." — jendela
  // diambil dari komentar penanda blok (posisi ALTER pertama bisa di akhir
  // blok, setelah array).
  auto src=read_src29("src/store/exam_store_postgres.cpp");
  auto marker=src.find("P21-T1: admin_users");
  ASSERT_NE(marker, std::string::npos)
    << "migrate() wajib punya blok ALTER admin_users (kontrak T1)";
  auto alter=src.find("ALTER TABLE admin_users ADD COLUMN IF NOT EXISTS ", marker);
  ASSERT_NE(alter, std::string::npos)
    << "blok harus mengeksekusi ALTER TABLE admin_users ADD COLUMN IF NOT EXISTS";
  // Jendela: dari marker sampai exec_command yang mengkonsumsi col_type.
  auto exec_pos=src.find("exec_command(std::string(\"ALTER TABLE admin_users ADD COLUMN IF NOT EXISTS \")", marker);
  ASSERT_NE(exec_pos, std::string::npos);
  auto block=src.substr(marker, exec_pos+120-marker);
  for(const char* col : {"name","email","password_hash","role","instansi","status",
                         "otp_code","otp_expiry","otp_attempts","package",
                         "max_exams","max_pdf_size","max_concurrent_exams",
                         "max_storage_size","expires_at","registered_ip",
                         "operator_created","base_role","package_role",
                         "whatsapp_number"}){
    EXPECT_NE(block.find(std::string("\"")+col+" "), std::string::npos)
      << "fresh-DB: admin_users versi migrate wajib punya kolom '" << col
      << "' (dipakai INSERT register/edit_user)";
  }
}

// ===== T2: pool PG proses-wide ==========================================

TEST(P29, GlobalPoolExists){
  auto hpp=read_src29("src/db/pool_global.hpp");
  EXPECT_NE(hpp.find("global_pool"), std::string::npos)
    << "wajib ada pool PG proses-wide (examvan::db::global_pool) — kontrak T2";
}

TEST(P29, WithPgUsesGlobalPool){
  // Semua duplikat with_pg di handler admin harus memakai pool proses-wide
  // (dengan_global_pg/global_pool dari db/pool_global.hpp), BUKAN membangun
  // RealPool stack-lokal per request (koneksi TCP+auth PG baru tiap request,
  // lalu PQfinish di dtor).
  for(const char* f : {"src/handlers/admin/pengawas.cpp",
                       "src/handlers/admin/submissions.cpp",
                       "src/handlers/admin/settings.cpp",
                       "src/handlers/admin/vouchers.cpp",
                       "src/handlers/admin/users.cpp"}){
    auto src=read_src29(f);
    bool uses_global=src.find("with_global_pg")!=std::string::npos ||
                     src.find("db::global_pool")!=std::string::npos;
    EXPECT_TRUE(uses_global) << f << " harus memakai pool global (T2)";
    EXPECT_EQ(src.find("RealPool real("), std::string::npos)
      << f << " tidak boleh membangun RealPool stack-lokal per panggilan (T2)";
  }
}

// ===== T3: protobuf-helper CDN fallback =================================

TEST(P29, ProtobufHelperNoCdnBranch){
  auto js=read_src29("static/js/protobuf-helper.js");
  EXPECT_EQ(js.find("unpkg.com"), std::string::npos)
    << "T3: branch CDN dengan SRI placeholder tidak pernah berhasil; bundle lokal "
       "/static/js/protobuf.min.js juga tidak ada. Hapus branch CDN.";
}

// ===== T4: versi satu sumber ============================================

TEST(P29, VersionSingleSource){
  // Satu-satunya literal versi boleh di default config.hpp.
  auto cfg=read_src29("src/config/config.hpp");
  EXPECT_NE(cfg.find("2.7.2"), std::string::npos)
    << "default versi tetap di config.hpp (satu sumber)";
  for(const char* f : {"src/handlers/admin/dashboard.cpp",
                       "src/handlers/admin/pengawas.cpp",
                       "src/handlers/admin/settings.cpp",
                       "src/handlers/admin/submissions.cpp",
                       "src/handlers/api/exams.cpp",
                       "src/handlers/auth/template_renderer.cpp",
                       "src/handlers/public/download.cpp",
                       "src/handlers/public/hasil.cpp"}){
    auto src=read_src29(f);
    EXPECT_EQ(src.find("2.7.2"), std::string::npos)
      << f << " tidak boleh hardcode versi — pakai Config::load().version (T4)";
  }
}

TEST(P29, DownloadApkUsesConfigVersion){
  // Object key R2 APK mengikuti Config::version, bukan literal — update APK
  // tidak boleh butuh recompile.
  auto src=read_src29("src/handlers/public/download.cpp");
  EXPECT_NE(src.find("Config::load().version"), std::string::npos)
    << "download_apk wajib pakai versi runtime dari Config (T4)";
  EXPECT_EQ(src.find("object_key_for_app(\"2.7.2\""), std::string::npos)
    << "object key R2 APK tidak boleh literal 2.7.2 (T4)";
}

// ===== T5: failed-queue ditulis worker ==================================

TEST(P29, FailedQueueKeyExists){
  auto hpp=read_src29("src/queue/submission_queue.hpp");
  EXPECT_NE(hpp.find("kFailedQueueKey"), std::string::npos)
    << "konstanta failed-queue wajib ada (T5) — panel queue_status membacanya";
}

TEST(P29, WorkerPushesPermanentFailureToFailedQueue){
  auto src=read_src29("src/queue/submission_queue.cpp");
  // Job yang melewati kMaxRetries (gagal permanen) harus masuk failed queue
  // agar queue_status.failed bermakna.
  EXPECT_NE(src.find("kFailedQueueKey"), std::string::npos)
    << "worker wajib LPUSH job gagal-permanen ke failed queue (T5)";
}

TEST(P29, QueueStatusReadsFailedKey){
  auto src=read_src29("src/handlers/admin/submissions.cpp");
  EXPECT_NE(src.find("kFailedQueueKey"), std::string::npos)
    << "queue_status wajib membaca konstanta failed-queue (bukan literal duplikat)";
}

// ===== T7: X-User append dihapus ========================================

TEST(P29, DashboardNoXUserAppend){
  auto src=read_src29("src/handlers/admin/dashboard.cpp");
  EXPECT_EQ(src.find("X-User"), std::string::npos)
    << "T7: append header X-User ke HTML dashboard adalah debug leftover";
}

// ===== T9: voucher expires_at divalidasi ================================

TEST(P29, VoucherExpiryValidated){
  auto src=read_src29("src/handlers/admin/vouchers.cpp");
  // create_voucher + create_vouchers_batch wajib memvalidasi format
  // expires_at dan menolak dengan 400 (bukan 500 generik dari PG).
  EXPECT_NE(src.find("valid_expires_at"), std::string::npos)
    << "T9: butuh helper validasi format expires_at";
}
