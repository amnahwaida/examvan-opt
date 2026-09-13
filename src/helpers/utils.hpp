#pragma once
#include <string>
#include <chrono>
#include <optional>
#include <map>

namespace examvan::helpers {

std::string format_iso_utc(std::chrono::system_clock::time_point tp);
std::optional<std::chrono::system_clock::time_point> parse_iso_utc(const std::string& s);
/* Parse timestamptz PG ("YYYY-MM-DD HH:MM:SS[.fff][+HH|+HH:MM|+HHMM]") maupun
 * ISO lama — dipakai gate expiry voucher (E-f): parse gagal → nullopt. */
std::optional<std::chrono::system_clock::time_point> parse_pg_or_iso_utc(const std::string& s);
std::string sanitize_student_input(const std::string& s);
std::string generate_token(int len = 8);
std::string localize_utc(const std::string& utc_str, int offset_minutes = 420);
bool is_valid_exam_token(const std::string& t);
std::string round_to(double v, int decimals);
/* Whitelist versi [A-Za-z0-9.-] panjang 1..64 — mencegah attribute breakout
 * di href asset saat menerima header X-Version klien (F4). */
bool is_safe_version(const std::string& s);

/* application/x-www-form-urlencoded: + → spasi, %XX → byte.
 * Wajib sebelum parse field body POST (csrf/username/password dsb). */
std::string url_decode(const std::string& s);

/* Parse body form ke map key→value (setelah url_decode). */
std::map<std::string,std::string> parse_form(const std::string& body);

/* "YYYY-MM-DD HH:MM[:SS]" (WIB, UTC+7) → "YYYY-MM-DDTHH:MM:SSZ" (UTC) —
 * konversi jadwal soal ujian (paritas Go SaveQuestions). P36-D6: parser
 * ketat — full-consumption + hari-dalam-bulan valid; nullopt bila format
 * menyimpang (dulu sscanf 5 field tanpa %n: "10:00JUNK" diterima,
 * "2026-02-31" menjadi 3 Maret). */
std::optional<std::string> wib_to_utc_iso(const std::string& s);

} // namespace examvan::helpers
