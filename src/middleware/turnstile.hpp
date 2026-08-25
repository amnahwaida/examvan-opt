#pragma once
#include <string>
namespace examvan::middleware {
bool verify_turnstile(const std::string& token, const std::string& secret, const std::string& remote_ip);
} // namespace examvan::middleware
