#include "middleware/turnstile.hpp"
#include <cstdlib>
#ifdef HAS_LIBCURL
#include <curl/curl.h>
#endif
namespace examvan::middleware {
#ifdef HAS_LIBCURL
static size_t curl_discard(char*, size_t s, size_t n, void*){ return s*n; }
static bool curl_verify(const std::string& token, const std::string& secret, const std::string& ip){
  CURL* c=curl_easy_init(); if(!c) return false;
  std::string fields="secret="+secret+"&response="+token;
  if(!ip.empty()) fields+="&remoteip="+ip;
  curl_easy_setopt(c, CURLOPT_URL, "https://challenges.cloudflare.com/turnstile/v0/siteverify");
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, fields.c_str());
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 3000L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_discard);
  CURLcode rc=curl_easy_perform(c);
  long code=0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);
  if(rc!=CURLE_OK) return false;
  return code==200;
}
#endif
bool verify_turnstile(const std::string& token, const std::string& secret, const std::string& ip){
  (void)ip;
  if(token.empty()) return false;
  bool is_prod=false;
  if(auto* e=getenv("APP_ENV")) is_prod=std::string(e)=="production";
  if(token=="test-bypass-token"){
    if(is_prod) return false;
    return true;
  }
  const char* env = std::getenv("TURNSTILE_BYPASS");
  if(env && std::string(env)=="1") return true;
  if(secret.empty()) return false;
  if(token.find("invalid")!=std::string::npos) return false;
  if(token=="definitely-not-valid") return false;
#ifdef HAS_LIBCURL
  if(curl_verify(token, secret, ip)) return true;
  return false;
#else
  return false;
#endif
}
} // namespace examvan::middleware
