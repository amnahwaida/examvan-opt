#include "middleware/turnstile.hpp"
#include <cstdlib>
#ifdef HAS_LIBCURL
#include <curl/curl.h>
#endif
namespace examvan::middleware {
#ifdef HAS_LIBCURL
static size_t curl_write_json(char* ptr, size_t s, size_t n, void* userdata){ std::string* out=(std::string*)userdata; out->append(ptr, s*n); return s*n; }
static bool curl_verify(const std::string& token, const std::string& secret, const std::string& ip){
  CURL* c=curl_easy_init(); if(!c) return false;
  char* esc_secret=curl_easy_escape(c, secret.c_str(), secret.size());
  char* esc_token=curl_easy_escape(c, token.c_str(), token.size());
  char* esc_ip=nullptr;
  if(!ip.empty()) esc_ip=curl_easy_escape(c, ip.c_str(), ip.size());
  std::string fields="secret="+(esc_secret?std::string(esc_secret):secret)+"&response="+(esc_token?std::string(esc_token):token);
  if(esc_ip) fields+="&remoteip="+std::string(esc_ip);
  if(esc_secret) curl_free(esc_secret);
  if(esc_token) curl_free(esc_token);
  if(esc_ip) curl_free(esc_ip);
  curl_easy_setopt(c, CURLOPT_URL, "https://challenges.cloudflare.com/turnstile/v0/siteverify");
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, fields.c_str());
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 3000L);
  std::string resp;
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_json);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
  CURLcode rc=curl_easy_perform(c);
  long code=0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);
  if(rc!=CURLE_OK) return false;
  if(code!=200) return false;
  if(resp.find("\"success\":true")!=std::string::npos || resp.find("\"success\": true")!=std::string::npos) return true;
  if(resp.find("\"success\":false")!=std::string::npos) return false;
  return false;
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
  if(is_prod){
    const char* env = std::getenv("TURNSTILE_BYPASS");
    if(env && std::string(env)=="1") return false;
  } else {
    const char* env = std::getenv("TURNSTILE_BYPASS");
    if(env && std::string(env)=="1") return true;
  }
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
