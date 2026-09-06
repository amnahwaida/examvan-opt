#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
#include <string>
#include <unordered_map>

namespace examvan::handlers::auth {

Response login_page(const Request& req);
Response login_handler(const Request& req, const Config& cfg);

// Build a signed session payload from verified user identity.
std::string build_login_session_payload(int admin_id, const std::string& username, const std::string& role_json);
std::string build_login_session_payload(int admin_id, const std::string& username, const std::string& role_json, const std::string& instansi);

void set_user_for_test(const std::string& username, const std::string& password_hash, const std::string& role);
void clear_users_for_test();
std::string get_csrf_for_test(const std::string& session_cookie);

} // namespace examvan::handlers::auth
