#pragma once
#include <string>

namespace examvan::helpers {

/* Kirim email via SMTP memakai libcurl (paritas Go helpers/email.go).
 * Mendukung TLS implisit (port 465), STARTTLS (587/25), dan PlainAuth bila
 * kredensial diisi. Mengembalikan string error (kosong = sukses). */
std::string send_smtp_message(const std::string& host,
                              const std::string& port,
                              const std::string& user,
                              const std::string& password,
                              const std::string& sender_name,
                              const std::string& to,
                              const std::string& subject,
                              const std::string& html_body);

std::string test_smtp_connection(const std::string& host, const std::string& port,
                                 const std::string& user, const std::string& password);

std::string send_verification_email(const std::string& host, const std::string& port,
                                    const std::string& user, const std::string& password,
                                    const std::string& sender_name, const std::string& to,
                                    const std::string& username, const std::string& otp);

std::string send_password_reset_email(const std::string& host, const std::string& port,
                                      const std::string& user, const std::string& password,
                                      const std::string& sender_name, const std::string& to,
                                      const std::string& username, const std::string& otp);

} // namespace examvan::helpers