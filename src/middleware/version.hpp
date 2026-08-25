#pragma once
#include "http/router.hpp"
#include <string>

namespace examvan::middleware {
bool is_version_allowed(const std::string& client_version, const std::string& required_version);
Response version_gate(const Request& req, const std::string& required, std::function<Response(const Request&)> next);
int compare_versions(const std::string& a, const std::string& b);
} // namespace examvan::middleware
