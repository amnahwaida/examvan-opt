#include "services/examtoken/examtoken.hpp"
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <sstream>
#include <iomanip>

namespace examvan::examtoken {

std::string hash_token(const std::string& token){
  unsigned char md[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), md);
  std::ostringstream ss; for(int i=0;i<SHA256_DIGEST_LENGTH;i++) ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)md[i];
  return ss.str();
}

bool verify_token(const std::string& token, const std::string& hash){
  return hash_token(token)==hash;
}

std::string generate_active_token(const std::string& base, const std::string& mode){
  if(mode=="static") return base;
  return base + "_" + std::to_string(time(nullptr)%10000);
}

} // namespace examvan::examtoken
