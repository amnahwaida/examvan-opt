#pragma once
#include "http/router.hpp"
namespace examvan::handlers::admin {
Response dashboard_page(const Request& req);
Response dashboard_stats(const Request& req);
} // namespace examvan::handlers::admin
