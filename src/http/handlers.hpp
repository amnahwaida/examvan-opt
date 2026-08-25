#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
namespace examvan {
void register_routes(Router& r, const Config& cfg);
Response health_handler(const Request& req, const Config& cfg);
}  // namespace examvan
