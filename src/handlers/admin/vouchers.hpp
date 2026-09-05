#pragma once
#include "http/router.hpp"
namespace examvan::handlers::admin {
Response list_vouchers(const Request& req);
Response create_voucher(const Request& req);
Response create_vouchers_batch(const Request& req);
Response toggle_voucher(const Request& req);
Response delete_voucher(const Request& req);
Response voucher_redemptions(const Request& req);
Response list_audit_logs(const Request& req);
Response vouchers_mine(const Request& req);
Response redeem_voucher(const Request& req);
Response activate_voucher(const Request& req);
Response list_packages(const Request& req);
Response save_packages(const Request& req);
Response billing_page(const Request& req);
} // namespace examvan::handlers::admin