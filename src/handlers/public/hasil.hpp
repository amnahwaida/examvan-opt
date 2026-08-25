#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
namespace examvan::handlers::public_ {
Response cek_hasil_page(const Request& req);
Response hasil_page(const Request& req);
Response cek_hasil_api(const Request& req);
} // namespace examvan::handlers::public_
