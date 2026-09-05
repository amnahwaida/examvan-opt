#pragma once
#include <string>

namespace examvan::helpers {

/* bcrypt ($2b$12) via crypt_r — kompatibel dengan hash Go (golang.org/x/crypto/bcrypt).
 * hash_password melempar std::runtime_error bila crypt_r gagal (RAND_bytes / salt). */
std::string hash_password(const std::string& plain);
bool verify_password(const std::string& plain, const std::string& hashed);

} // namespace examvan::helpers