#pragma once
#include <string>

namespace examvan {
std::string generate_csrf_token();
bool verify_csrf(const std::string& session_token, const std::string& request_token);
}  // namespace examvan
