#pragma once
#include "http/router.hpp"
#include <map>
#include <string>

namespace examvan::handlers::auth {

/* Helper bersama handler auth public (register/confirm/resend/forgot/reset).
 * Dipakai bersama login.cpp: verifikasi CSRF double-submit, IP klien dari
 * header proxy, deteksi klien JSON vs form HTML, dan halaman sukses. */

// Ambil header (case-insensitive). Kosong bila tidak ada.
std::string get_hdr_ci(const Request& req, const std::string& name);

// IP klien: X-Real-IP dulu, lalu X-Forwarded-For (di-set uWS di server.cpp),
// fallback "global". Dipakai sebagai kunci rate limit + daftar per-IP.
std::string client_ip(const Request& req);

// Cari token CSRF: header X-CSRF-Token/X-XSRF-Token → field form
// (csrf_token/_csrf/csrf) → field JSON. Dipanggil oleh semua mutasi public.
std::string request_csrf_token(const Request& req, const std::map<std::string,std::string>& form);

// Verifikasi CSRF double-submit: token dari body harus cocok dengan cookie
// csrf_token yang dikirim. Mengembalikan false bila cookie tidak ada.
bool csrf_ok(const Request& req, const std::map<std::string,std::string>& form);

// true bila klien meminta JSON (Accept / X-Requested-With / Content-Type).
bool wants_json(const Request& req);

// Response sukses: 303 (form HTML) atau 200 JSON — pola sama seperti login.
Response success_response(bool json, const std::string& location, const std::string& json_body);

// Helper url-encode sederhana (untuk query ?username= yang aman).
std::string url_encode(const std::string& s);

} // namespace examvan::handlers::auth