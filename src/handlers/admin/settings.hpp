#pragma once
#include "http/router.hpp"
namespace examvan::handlers::admin {
Response settings_page(const Request& req);
Response update_settings(const Request& req);
Response system_apps_page(const Request& req);
} // namespace examvan::handlers::admin
