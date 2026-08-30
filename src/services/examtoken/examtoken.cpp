#include "services/examtoken/examtoken.hpp"
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

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

// Go parity: examtoken.Matches() — case-insensitive; whitespace-trimmed.
// Dynamic: only active_token. Static: active_token OR permanent token.
// Empty token always rejected.
bool matches(const models::Exam& exam, const std::string& token){
  std::string t=token;
  // trim whitespace
  size_t b=t.find_first_not_of(" \t\r\n");
  size_t e=t.find_last_not_of(" \t\r\n");
  if(b==std::string::npos) t=""; else t=t.substr(b,e-b+1);
  if(t.empty()) return false;
  auto eq=[&](const std::string& ref)->bool{
    if(ref.empty()) return false;
    if(t.size()!=ref.size()) return false;
    for(size_t i=0;i<t.size();++i)
      if(toupper((unsigned char)t[i])!=toupper((unsigned char)ref[i])) return false;
    return true;
  };
  if(exam.get_token_mode()=="dynamic"){
    return eq(exam.active_token);
  }
  if(exam.active_token!="" && eq(exam.active_token)) return true;
  return exam.token!="" && eq(exam.token);
}

} // namespace examvan::examtoken
