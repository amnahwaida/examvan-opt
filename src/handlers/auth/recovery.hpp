#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
#include <string>

namespace examvan::handlers::auth {

/* Lupa/reset password public (paritas Go forgotPasswordPageHandler,
 * forgotPasswordPostHandler, resetPasswordPageHandler, resetPasswordPostHandler).
 * Semua mutasi wajib CSRF; handler di-rate-limit di router. */
Response forgot_password_page(const Request& req);
Response forgot_password_handler(const Request& req, const Config& cfg);
Response reset_password_page(const Request& req);
Response reset_password_handler(const Request& req, const Config& cfg);

} // namespace examvan::handlers::auth