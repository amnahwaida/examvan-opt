#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
namespace examvan::handlers::public_ {
Response download_page(const Request& req);
Response download_apk(const Request& req);
Response download_system_app(const Request& req);
} // namespace examvan::handlers::public_
