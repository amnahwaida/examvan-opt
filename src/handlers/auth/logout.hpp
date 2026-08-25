#pragma once
#include "http/router.hpp"
namespace examvan::handlers::auth {
Response logout_handler(const Request& req);
Response logout_page(const Request& req);
}
