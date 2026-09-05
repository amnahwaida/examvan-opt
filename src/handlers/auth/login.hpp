#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
#include <string>
#include <unordered_map>

namespace examvan::handlers::auth {

Response login_page(const Request& req);
Response login_handler(const Request& req, const Config& cfg);

// Bangun payload session (base64) dari id/role ASLI user — JANGAN pernah
// hardcode admin_id/role di sini (dulu admin_id=1&role=["guru"] di-forge
// untuk semua login → privilege escalation + created_by salah).
std::string build_login_session_payload(int admin_id, const std::string& username, const std::string& role_json);

void set_user_for_test(const std::string& username, const std::string& password_hash, const std::string& role);
void clear_users_for_test();
std::string get_csrf_for_test(const std::string& session_cookie);

} // namespace examvan::handlers::auth
