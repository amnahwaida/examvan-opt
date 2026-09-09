// Pass-24 TDD remediasi Batch 1 (HIGH) — RED phase.
// Temuan: docs/review-temuan-2026-09-09-pass24.md
//  - E-a: users.cpp 7 handler admin-write fake-200 saat PG down
//  - E-f: voucher_usable parse ISO-strict → expiry di-skip saat parse gagal
//  - E-g: update_settings swallow kegagalan PG → false 200
//  - E-h: create_voucher/toggle_voucher fake-200 saat PG down
//  - D2: delegate_exam transaksi unchecked + catch-all 200
//  - I1: nginx client_max_body_size 5m vs carve-out 105MB system-apps
//  - F1: webhook OTP early-ack, tanpa expiry/attempts, route tanpa rate-limit
//  - F4: header X-Version diterima mentah → attribute breakout di href asset
//
// Kontrak (RED dulu, GREEN setelah remediasi):
//  1. Ea  — 7 handler admin-write (edit/delete/toggle/verify/deactivate/
//           change_password/instansi_update) fail-closed 503 saat PG down.
//  2. Ef  — voucher_usable wajib fail-closed saat expiry tak terparse +
//           parser baru parse_pg_or_iso_utc menerima format timestamptz PG.
//  3. Eg  — update_settings wajib 503 saat upsert PG gagal.
//  4. Eh  — create_voucher + toggle_voucher fail-closed saat PG down.
//  5. D2  — delegate_exam fail-closed 503 saat transaksi PG gagal.
//  6. I1  — nginx carve-out 105m untuk /admin/api/system-apps.
//  7. F1  — ack webhook SETELAH validasi; OTP expiry + attempts + rate-limit.
//  8. F4  — X-Version hanya diterima jika is_safe_version (whitelist).
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>

#include "helpers/utils.hpp"

using namespace examvan;

namespace {

std::string read_src33(const std::string& p) {
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

// Kontrak generik fail-closed: window handler wajib punya gate pg_configured
// yang mengembalikan 503 "Database tidak tersedia" (bukan fake-200).
void expect_fail_closed(const std::string& file, const std::string& begin_marker,
                        const std::string& end_marker, const std::string& handler) {
  auto src = read_src33(file);
  ASSERT_FALSE(src.empty()) << file << " harus ada";
  auto seg = between(src, begin_marker, end_marker);
  ASSERT_FALSE(seg.empty()) << handler << " tidak ditemukan di " << file;
  SCOPED_TRACE(handler);
  EXPECT_NE(seg.find("pg_configured"), std::string::npos)
      << handler << " wajib gate pg_configured";
  EXPECT_NE(seg.find("503"), std::string::npos)
      << handler << " wajib 503 saat PG down";
  EXPECT_NE(seg.find("Database tidak tersedia"), std::string::npos)
      << handler << " tidak boleh fake-200 saat PG down";
}

}  // namespace

// E-a: 7 handler admin-write di users.cpp wajib fail-closed saat PG down.
TEST(P33, Ea_AdminWriteFailClosed) {
  const std::string f = "src/handlers/admin/users.cpp";
  expect_fail_closed(f, "Response edit_user(const Request& req){",
                     "Response delete_user(const Request& req){", "edit_user");
  expect_fail_closed(f, "Response delete_user(const Request& req){",
                     "Response user_toggle_status(const Request& req){", "delete_user");
  expect_fail_closed(f, "Response user_toggle_status(const Request& req){",
                     "Response user_verify(const Request& req){", "user_toggle_status");
  expect_fail_closed(f, "Response user_verify(const Request& req){",
                     "Response user_deactivate_package(const Request& req){", "user_verify");
  expect_fail_closed(f, "Response user_deactivate_package(const Request& req){",
                     "Response change_password(const Request& req){", "user_deactivate_package");
  expect_fail_closed(f, "Response change_password(const Request& req){",
                     "Response instansi_update(const Request& req){", "change_password");
  expect_fail_closed(f, "Response instansi_update(const Request& req){",
                     "} // namespace examvan::handlers::admin", "instansi_update");
}

// E-f: voucher_usable wajib fail-closed saat expiry tak terparse, dan parser
// baru parse_pg_or_iso_utc wajib menerima format timestamptz PG.
TEST(P33, Ef_VoucherUsableFailsClosedOnParseFailure) {
  auto src = read_src33("src/handlers/admin/vouchers.cpp");
  ASSERT_FALSE(src.empty());
  auto seg = between(src, "static bool voucher_usable", "static int duration_days");
  ASSERT_FALSE(seg.empty()) << "voucher_usable tidak ditemukan";
  EXPECT_NE(seg.find("parse_pg_or_iso_utc"), std::string::npos)
      << "voucher_usable wajib pakai parser PG-aware";
  EXPECT_NE(seg.find("if(!tp) return false;"), std::string::npos)
      << "parse gagal = voucher TIDAK usable (fail-closed), bukan skip expiry";
}

TEST(P33, Ef_ParsePgOrIsoUtc_Behaviour) {
  // Format timestamptz PG: "YYYY-MM-DD HH:MM:SS[.fff][+07|+07:00|+0700|Z]"
  EXPECT_TRUE(helpers::parse_pg_or_iso_utc("2026-09-09 12:34:56.789+07").has_value());
  EXPECT_TRUE(helpers::parse_pg_or_iso_utc("2026-09-09 12:34:56+00").has_value());
  // Format ISO lama tetap diterima.
  EXPECT_TRUE(helpers::parse_pg_or_iso_utc("2026-09-09T12:34:56Z").has_value());
  // String sampah wajib nullopt.
  EXPECT_FALSE(helpers::parse_pg_or_iso_utc("not-a-date").has_value());
}

// E-g: update_settings wajib 503 saat upsert PG gagal (bukan false-200).
TEST(P33, Eg_UpdateSettingsFailsClosed) {
  expect_fail_closed("src/handlers/admin/settings.cpp", "Response update_settings",
                     "Response test_smtp_connection", "update_settings");
}

// E-h: create_voucher + toggle_voucher wajib fail-closed saat PG down.
TEST(P33, Eh_VoucherHandlersFailClosed) {
  const std::string f = "src/handlers/admin/vouchers.cpp";
  expect_fail_closed(f, "Response toggle_voucher(", "Response delete_voucher(",
                    "toggle_voucher");
  expect_fail_closed(f, "Response create_voucher(", "// ===== batch create",
                    "create_voucher");
}

// D2: delegate_exam wajib fail-closed saat transaksi PG gagal (bukan
// catch-all 200 "Delegasi ujian berhasil disimpan").
TEST(P33, D2_DelegateExamFailsClosed) {
  expect_fail_closed("src/handlers/admin/exams.cpp", "Response delegate_exam(",
                     "} // namespace examvan::handlers::admin", "delegate_exam");
}

// I1: nginx wajib carve-out client_max_body_size 105m untuk
// /admin/api/system-apps (paritas dengan admin_api wrapper 105 MB).
TEST(P33, I1_NginxSystemAppsBodySize) {
  auto src = read_src33("nginx/nginx.conf");
  ASSERT_FALSE(src.empty());
  auto seg = between(src, "location /admin/api/system-apps", "}");
  ASSERT_FALSE(seg.empty())
      << "location /admin/api/system-apps wajib ada di nginx.conf";
  EXPECT_NE(seg.find("client_max_body_size 105m"), std::string::npos)
      << "carve-out system-apps wajib 105m (bukan default 5m http-level)";
}

// F1(a): ack webhook (set_success(true)) hanya SETELAH validasi — early-ack
// protobuf wajib dihapus.
TEST(P33, F1_WebhookAckAfterValidation) {
  auto src = read_src33("src/handlers/api/webhook.cpp");
  ASSERT_FALSE(src.empty());
  auto seg = between(src, "Response webhook(", "// Parse sender/message");
  ASSERT_FALSE(seg.empty()) << "window awal handler webhook tidak ditemukan";
  EXPECT_EQ(seg.find("set_success(true)"), std::string::npos)
      << "ack wajib setelah validasi — hapus early-ack protobuf";
}

// F1(b)+(c): OTP wajib cek expiry + attempts (bump_otp_attempts, mirroring
// recovery.cpp) dan SELECT wajib per-user, bukan scan seluruh admin_users.
TEST(P33, F1_WebhookOtpExpiryAndAttempts) {
  auto src = read_src33("src/handlers/api/webhook.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_EQ(src.find("SELECT id, username, whatsapp_number, status, otp_code FROM admin_users"),
            std::string::npos)
      << "SELECT lama (scan seluruh tabel) wajib diganti per-user";
  EXPECT_NE(src.find("bump_otp_attempts"), std::string::npos)
      << "webhook wajib bump attempts OTP via auth_store";
  EXPECT_NE(src.find("Terlalu banyak percobaan"), std::string::npos)
      << "limit attempts OTP wajib ditegakkan";
  EXPECT_NE(src.find("Kode OTP sudah kedaluwarsa"), std::string::npos)
      << "expiry OTP wajib ditegakkan";
}

// F1(d): route /api/webhook wajib di-rate-limit (anti brute-force OTP).
TEST(P33, F1_WebhookRouteRateLimited) {
  auto src = read_src33("src/http/router_full.cpp");
  ASSERT_FALSE(src.empty());
  EXPECT_NE(src.find("r.add(\"POST\",\"/api/webhook\", rl_wrap("), std::string::npos)
      << "route webhook wajib dibungkus rl_wrap (RateLimiter)";
}

// F4: is_safe_version — whitelist charset [A-Za-z0-9.-], panjang 1..64.
TEST(P33, F4_IsSafeVersion_Behaviour) {
  EXPECT_TRUE(helpers::is_safe_version("2.7.3-983a6cca"));
  EXPECT_FALSE(helpers::is_safe_version("2.7.3\" onclick=x"));
  EXPECT_FALSE(helpers::is_safe_version(""));
  EXPECT_FALSE(helpers::is_safe_version(std::string(65, 'v')));
}

// F4: acceptance point hasil.cpp — X-Version hanya dipakai jika safe.
TEST(P33, F4_HasilUsesSafeVersion) {
  auto src = read_src33("src/handlers/public/hasil.cpp");
  ASSERT_FALSE(src.empty());
  auto seg = between(src, "auto it=req.headers.find(\"X-Version\")",
                     "render_public_template");
  ASSERT_FALSE(seg.empty()) << "window X-Version tidak ditemukan";
  EXPECT_NE(seg.find("is_safe_version"), std::string::npos)
      << "X-Version klien hanya diterima jika is_safe_version";
}
