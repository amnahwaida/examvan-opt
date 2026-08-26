#pragma once
#include <string>
#include <chrono>
#include <optional>
#include <map>

namespace examvan::helpers {

std::string format_iso_utc(std::chrono::system_clock::time_point tp);
std::optional<std::chrono::system_clock::time_point> parse_iso_utc(const std::string& s);
std::string sanitize_student_input(const std::string& s);
std::string generate_token(int len = 8);
std::string localize_utc(const std::string& utc_str, int offset_minutes = 420);
bool is_valid_exam_token(const std::string& t);
std::string round_to(double v, int decimals);

/* application/x-www-form-urlencoded: + → spasi, %XX → byte.
 * Wajib sebelum parse field body POST (csrf/username/password dsb). */
std::string url_decode(const std::string& s);

/* Parse body form ke map key→value (setelah url_decode). */
std::map<std::string,std::string> parse_form(const std::string& body);

} // namespace examvan::helpers
