#pragma once
#include <string>

namespace examvan {

std::string sanitize_ws_field(const std::string& raw, size_t max_len);
std::string sanitize_ws_mac(const std::string& raw);
std::string ws_string(const std::string& json_obj, const std::string& key);
std::string html_escape(const std::string& s);
// Buang pasangan "key"/"answer" dari JSON soal secara JSON-aware (nilai bisa
// string, array, objek, atau angka). Dipakai endpoint publik supaya kunci
// jawaban tidak bocor (paritas Go stripAnswerKeys).
std::string strip_sensitive_keys(const std::string& questions_json);

}  // namespace examvan
