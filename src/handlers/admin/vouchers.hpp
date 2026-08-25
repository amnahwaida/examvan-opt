#pragma once
#include "http/router.hpp"
namespace examvan::handlers::admin {
Response list_vouchers(const Request& req);
Response redeem_voucher(const Request& req);
Response activate_voucher(const Request& req);
Response billing_page(const Request& req);
} // namespace examvan::handlers::admin
