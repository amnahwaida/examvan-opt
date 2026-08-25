#pragma once
#include "http/router.hpp"
namespace examvan::middleware {
Response body_limit(const Request& req, size_t max_bytes, std::function<Response(const Request&)> next);
} // namespace examvan::middleware
