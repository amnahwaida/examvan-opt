#pragma once
#include "http/router.hpp"
#include "config/config.hpp"
#include <string>

namespace examvan::handlers::auth {

/* Halaman + handler pendaftaran public (paritas Go registerPostHandler,
 * registerConfirmPageHandler, registerConfirmPostHandler, resendOTPHandler).
 * Semua mutasi wajib CSRF; handler di-rate-limit di router. */

Response register_page(const Request& req);
Response register_handler(const Request& req, const Config& cfg);
Response register_confirm_page(const Request& req);
Response register_confirm_handler(const Request& req, const Config& cfg);
Response resend_otp(const Request& req, const Config& cfg);

/* Hook unit test: registrasi berjalan terhadap peta in-memory (bukan PG)
 * sehingga suite tidak butuh database. */
void set_registered_user_for_test(const std::string& username,
                                  const std::string& email,
                                  const std::string& password_hash,
                                  const std::string& status,
                                  const std::string& otp_code,
                                  int otp_attempts,
                                  long otp_expiry_epoch_sec);
void clear_registered_users_for_test();

/* Reset rate-limiter register per-IP (statis) — dipanggil antar-test agar
 * suite tidak kehabisan kuota 5/jam dan test CSRF tidak kena 429. */
void reset_register_limit_for_test();

} // namespace examvan::handlers::auth