#pragma once
#include "http/router.hpp"
namespace examvan::middleware {
bool is_origin_allowed(const std::string& origin, const std::string& allowed_csv);
Response cors_wrap(const Request& req, const std::string& allowed_csv, std::function<Response(const Request&)> next);
} // namespace examvan::middleware
