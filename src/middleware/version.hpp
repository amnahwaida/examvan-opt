#pragma once
#include "http/router.hpp"
#include <string>
#include <optional>

namespace examvan::middleware {
bool is_version_allowed(const std::string& client_version, const std::string& required_version);
Response version_gate(const Request& req, const std::string& required, std::function<Response(const Request&)> next);
int compare_versions(const std::string& a, const std::string& b);

/* Semantik AndroidVersionCheck Go (middleware/version.go):
 * 1) client KOSONG  → false (client web tanpa header diizinkan)
 * 2) required KOSONG → false (belum ada APK terbit di system_apps)
 * 3) keduanya ada    → blok bila client < required (426). */
bool should_block_version(const std::string& client, const std::string& required);
} // namespace examvan::middleware
