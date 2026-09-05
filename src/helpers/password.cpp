#include "helpers/password.hpp"
#include <openssl/rand.h>
#include <crypt.h>
#include <stdexcept>

namespace examvan::helpers {

static std::string gensalt(){ // bcrypt gensalt
  unsigned char buf[16];
  if(RAND_bytes(buf,sizeof(buf))!=1){ throw std::runtime_error("RAND_bytes failed"); }
  const char* b64="./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::string s;
  s.reserve(22);
  int v=0, bits=0;
  for(int i=0;i<16;i++){
    v = (v<<8)|buf[i];
    bits+=8;
    while(bits>=6){ bits-=6; s.push_back(b64[(v>>bits)&0x3F]); }
  }
  if(bits>0) s.push_back(b64[(v<<(6-bits))&0x3F]);
  if(s.size()>22) s.resize(22);
  return s;
}

std::string hash_password(const std::string& p){
  std::string salt="$2b$12$"+gensalt();
  struct crypt_data cd{}; cd.initialized=0;
  char* out=crypt_r(p.c_str(), salt.c_str(), &cd);
  if(out) return std::string(out);
  throw std::runtime_error("crypt_r failed");
}

bool verify_password(const std::string& plain, const std::string& hashed){
  if(hashed.rfind("$2b$",0)==0 || hashed.rfind("$2a$",0)==0 || hashed.rfind("$2y$",0)==0){
    struct crypt_data cd{}; cd.initialized=0;
    char* out=crypt_r(plain.c_str(), hashed.c_str(), &cd);
    if(!out) return false;
    if(std::string(out).size()!=hashed.size()) return false;
    volatile int d=0;
    for(size_t i=0;i<hashed.size();i++) d|=out[i]^hashed[i];
    return d==0;
  }
  return false;
}

} // namespace examvan::helpers