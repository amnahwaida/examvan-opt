#pragma once
#include "http/router.hpp"
#include "session/cookie.hpp"
#include <string>

namespace examvan::middleware {

constexpr const char* kSessionCookie = "examvan_session";
constexpr const char* kSessionKeyAdminId = "admin_id";

bool is_authenticated(const Request& req, const std::string& secret, SessionData* out = nullptr);
Response require_auth(const Request& req, const std::string& secret, std::function<Response(const Request&, const SessionData&)> next);
bool require_role(const SessionData& s, const std::string& role);

} // namespace examvan::middleware
