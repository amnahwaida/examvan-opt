#include "session/csrf.hpp"
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <string>

namespace examvan {

std::string generate_csrf_token() {
  unsigned char buf[32];
  RAND_bytes(buf, sizeof(buf));
  std::string raw(reinterpret_cast<char*>(buf), sizeof(buf));
  std::string out(4 * ((raw.size()+2)/3)+1, '\0');
  int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                          reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
  out.resize(n);
  return out;
}

bool verify_csrf(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return false;
  if (a.size()!=b.size()) return false;
  volatile int diff=0;
  for (size_t i=0;i<a.size();++i) diff |= a[i]^b[i];
  return diff==0;
}

}  // namespace examvan
