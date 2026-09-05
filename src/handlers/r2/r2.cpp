#include "handlers/r2/r2.hpp"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
#ifdef HAS_LIBCURL
#include <curl/curl.h>
#endif

namespace examvan::r2 {

static std::string uri_encode(const std::string& s){
  std::ostringstream o;
  o<<std::hex<<std::uppercase;
  for(unsigned char c: s){
    if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') o<<c;
    else o<<'%'<<std::setw(2)<<std::setfill('0')<<(int)c;
  }
  return o.str();
}
static std::string region_for_endpoint(const std::string& endpoint){
  std::string h=endpoint;
  auto p=h.find("://");
  if(p!=std::string::npos) h=h.substr(p+3);
  h=h.substr(0,h.find('/'));
  if(h.find("r2.cloudflarestorage.com")!=std::string::npos) return "auto";
  return "auto";
}
static std::string sha256_hex(const std::string& s){
  unsigned char h[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), h);
  std::ostringstream o; for(int i=0;i<SHA256_DIGEST_LENGTH;i++) o<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)h[i];
  return o.str();
}
std::string presign_url(const R2Config& cfg, const std::string& key, int expires_seconds){
  if(!cfg.enabled()) return "";
  if(cfg.bucket.empty()) return "";
  // M10: fail-closed sama seperti upload/verify/remove — endpoint non-R2
  // (mis. salah konfigurasi) TIDAK boleh menghasilkan URL yang menunjuk host
  // lain. Tanpa cek ini, presigned URL diam-diam mengarah ke host attacker.
  if(cfg.endpoint.find("r2.cloudflarestorage.com")==std::string::npos) return "";
  if(expires_seconds<=0 || expires_seconds>7*24*3600) expires_seconds=3600;
  std::time_t now=time(nullptr);
  char date_full[32];
  {
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm,&now);
#else
    gmtime_r(&now,&tm);
#endif
    std::strftime(date_full,sizeof(date_full),"%Y%m%dT%H%M%SZ", &tm);
  }
  std::string date8(date_full,8);
  std::string region = region_for_endpoint(cfg.endpoint);
  std::string credential_raw = cfg.access_key + "/" + date8 + "/" + region + "/s3/aws4_request";
  std::string credential = uri_encode(credential_raw);
  std::string host = cfg.endpoint;
  auto p = host.find("://");
  if(p!=std::string::npos) host = host.substr(p+3);
  host = host.substr(0, host.find('/'));
  std::string encoded_key;
  {
    std::ostringstream ko;
    ko<<std::hex<<std::uppercase;
    for(size_t i=0;i<key.size();){
      if(key[i]=='/'){ ko<<'/'; i++; continue; }
      unsigned char c=key[i];
      if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') ko<<c;
      else ko<<'%'<<std::setw(2)<<std::setfill('0')<<(int)c;
      i++;
    }
    encoded_key=ko.str();
  }
  std::string canonical_uri = "/" + cfg.bucket + "/" + encoded_key;
  std::string canonical_qs = "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=" + credential + "&X-Amz-Date=" + std::string(date_full) + "&X-Amz-Expires=" + std::to_string(expires_seconds) + "&X-Amz-SignedHeaders=host";
  std::string canonical_headers = "host:" + host + "\n";
  std::string signed_headers = "host";
  std::string payload_hash = "UNSIGNED-PAYLOAD";
  std::string canonical_request = "GET\n" + canonical_uri + "\n" + canonical_qs + "\n" + canonical_headers + "\n" + signed_headers + "\n" + payload_hash;
  std::string hashed_canonical = sha256_hex(canonical_request);
  std::string string_to_sign = "AWS4-HMAC-SHA256\n" + std::string(date_full) + "\n" + date8 + "/" + region + "/s3/aws4_request\n" + hashed_canonical;
  auto hmac = [](const std::string& k, const std::string& d){ unsigned char md[EVP_MAX_MD_SIZE]; unsigned int l=0; HMAC(EVP_sha256(), k.data(), k.size(), reinterpret_cast<const unsigned char*>(d.data()), d.size(), md, &l); return std::string(reinterpret_cast<char*>(md), l); };
  std::string kDate = hmac("AWS4"+cfg.secret_key, date8);
  std::string kRegion = hmac(kDate, region);
  std::string kService = hmac(kRegion, "s3");
  std::string kSigning = hmac(kService, "aws4_request");
  unsigned char sig_md[EVP_MAX_MD_SIZE]; unsigned int sig_len=0;
  HMAC(EVP_sha256(), kSigning.data(), kSigning.size(), reinterpret_cast<const unsigned char*>(string_to_sign.data()), string_to_sign.size(), sig_md, &sig_len);
  std::ostringstream sig; for(unsigned i=0;i<sig_len;i++) sig<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)sig_md[i];
  std::ostringstream ss;
  std::string endpoint_no_slash=cfg.endpoint;
  if(!endpoint_no_slash.empty() && endpoint_no_slash.back()=='/') endpoint_no_slash.pop_back();
  ss<< endpoint_no_slash << "/" << cfg.bucket << "/" << encoded_key
    << "?" << canonical_qs
    << "&X-Amz-Signature=" << sig.str();
  return ss.str();
}

std::string object_key_for_exam(int exam_id, const std::string& filename){
  return "exams/" + std::to_string(exam_id) + "/" + filename;
}

std::string object_key_for_app(const std::string& version, const std::string& flavor){
  return "apps/android/" + version + "/EXAMVAN-v" + version + "-" + flavor + ".apk";
}

#ifdef HAS_LIBCURL
namespace {
static std::string hmac_bin(const std::string& key, const std::string& data){
  unsigned char md[EVP_MAX_MD_SIZE]; unsigned int l=0;
  HMAC(EVP_sha256(), key.data(), key.size(), reinterpret_cast<const unsigned char*>(data.data()), data.size(), md, &l);
  return std::string(reinterpret_cast<char*>(md), l);
}
static void curl_append_header(std::string& auth_out, std::string& date_full_out, std::string& date8_out,
                               const R2Config& cfg, const std::string& method,
                               const std::string& canonical_uri, const std::string& payload_hash,
                               const std::string& host){
  std::time_t now=time(nullptr);
  char df[32];
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm,&now);
#else
  gmtime_r(&now,&tm);
#endif
  std::strftime(df,sizeof(df),"%Y%m%dT%H%M%SZ",&tm);
  date_full_out=df;
  date8_out=std::string(df,8);
  std::string region=region_for_endpoint(cfg.endpoint);
  std::string canonical_headers="host:"+host+"\n"+"x-amz-content-sha256:"+payload_hash+"\n"+"x-amz-date:"+date_full_out+"\n";
  std::string signed_headers="host;x-amz-content-sha256;x-amz-date";
  std::string canonical_request=method+"\n"+canonical_uri+"\n\n"+canonical_headers+"\n"+signed_headers+"\n"+payload_hash;
  std::string hashed_canonical=sha256_hex(canonical_request);
  std::string scope=date8_out+"/"+region+"/s3/aws4_request";
  std::string string_to_sign="AWS4-HMAC-SHA256\n"+date_full_out+"\n"+scope+"\n"+hashed_canonical;
  std::string kDate=hmac_bin("AWS4"+cfg.secret_key, date8_out);
  std::string kRegion=hmac_bin(kDate, region);
  std::string kService=hmac_bin(kRegion, "s3");
  std::string kSigning=hmac_bin(kService, "aws4_request");
  unsigned char sig_md[EVP_MAX_MD_SIZE]; unsigned int sig_len=0;
  HMAC(EVP_sha256(), kSigning.data(), kSigning.size(), reinterpret_cast<const unsigned char*>(string_to_sign.data()), string_to_sign.size(), sig_md, &sig_len);
  std::ostringstream sig; for(unsigned i=0;i<sig_len;i++) sig<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)sig_md[i];
  auth_out="AWS4-HMAC-SHA256 Credential="+cfg.access_key+"/"+scope+", SignedHeaders="+signed_headers+", Signature="+sig.str();
}
static std::string build_url(const R2Config& cfg, const std::string& key, std::string& host_out, std::string& canonical_uri_out){
  std::string host=cfg.endpoint;
  auto p=host.find("://");
  if(p!=std::string::npos) host=host.substr(p+3);
  host=host.substr(0, host.find('/'));
  host_out=host;
  std::string encoded_key;
  {
    std::ostringstream ko; ko<<std::hex<<std::uppercase;
    for(size_t i=0;i<key.size();){
      if(key[i]=='/'){ ko<<'/'; i++; continue; }
      unsigned char c=key[i];
      if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') ko<<c;
      else ko<<'%'<<std::setw(2)<<std::setfill('0')<<(int)c;
      i++;
    }
    encoded_key=ko.str();
  }
  canonical_uri_out="/"+cfg.bucket+"/"+encoded_key;
  std::string endpoint_no_slash=cfg.endpoint;
  if(!endpoint_no_slash.empty() && endpoint_no_slash.back()=='/') endpoint_no_slash.pop_back();
  return endpoint_no_slash+"/"+cfg.bucket+"/"+encoded_key;
}
static size_t curl_read_cb(char* ptr, size_t size, size_t nmemb, void* userdata){
  auto* ctx=(std::pair<const std::string*,size_t>*)userdata;
  const std::string* data=ctx->first;
  size_t& off=ctx->second;
  size_t want=size*nmemb;
  size_t remain=data->size()>off? data->size()-off:0;
  size_t n=std::min(want, remain);
  if(n) { memcpy(ptr, data->data()+off, n); off+=n; }
  return n;
}
static size_t curl_discard_cb(char* ptr, size_t size, size_t nmemb, void*){ return size*nmemb; }
} // anon
#endif

bool R2Client::upload(const std::string& key, const std::string& data, const std::string& content_type) const {
  if(!enabled()) return false;
  if(cfg.bucket.empty()) return false;
  // Fail-closed: satu-satunya sukses-palsu adalah opt-in EKSPLISIT
  // EXAMVAN_R2_TESTMODE=1 (hanya test harness/operator yang sadar, bukan
  // default produksi). Tanpa itu: tanpa libcurl atau endpoint non-R2 = GAGAL.
  const char* tm=std::getenv("EXAMVAN_R2_TESTMODE");
  if(tm && std::string(tm)=="1") return true;
#ifndef HAS_LIBCURL
  return false;
#else
  if(cfg.endpoint.find("r2.cloudflarestorage.com")==std::string::npos) return false;
  std::string payload_hash=sha256_hex(data);
  std::string host, canonical_uri;
  std::string url=build_url(cfg, key, host, canonical_uri);
  auto write_cb = [](char* ptr, size_t s, size_t n, void* u)->size_t{ auto* str=(std::string*)u; str->append(ptr,s*n); return s*n; };
  // Retry (transient network error): maksimal 3 percobaan dengan backoff kecil.
  const int kMaxAttempts=3;
  std::string resp_body;
  CURLcode rc=CURLE_FAILED_INIT;
  long code=0;
  for(int attempt=1; attempt<=kMaxAttempts; ++attempt){
    std::string auth, date_full, date8;
    curl_append_header(auth, date_full, date8, cfg, "PUT", canonical_uri, payload_hash, host);
    CURL* c=curl_easy_init();
    if(!c) return false;
    struct curl_slist* hdrs=nullptr;
    hdrs=curl_slist_append(hdrs, ("Authorization: "+auth).c_str());
    hdrs=curl_slist_append(hdrs, ("x-amz-date: "+date_full).c_str());
    hdrs=curl_slist_append(hdrs, ("x-amz-content-sha256: "+payload_hash).c_str());
    hdrs=curl_slist_append(hdrs, ("Content-Type: "+content_type).c_str());
    hdrs=curl_slist_append(hdrs, "Expect:");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, curl_read_cb);
    std::pair<const std::string*,size_t> ctx{&data,0};
    curl_easy_setopt(c, CURLOPT_READDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)data.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp_body);
    rc=curl_easy_perform(c);
    code=0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    if(rc==CURLE_OK && code>=200 && code<300) return true;
    if(rc!=CURLE_OK){
      fprintf(stderr,"[r2] upload curl %d %s url=%s code=%ld resp=%.200s (attempt %d/%d)\n",(int)rc,curl_easy_strerror(rc),url.c_str(),code,resp_body.c_str(),attempt,kMaxAttempts);
    } else {
      fprintf(stderr,"[r2] upload http %ld url=%s resp=%.500s auth=%.80s (attempt %d/%d)\n",code,url.c_str(),resp_body.c_str(),auth.c_str(),attempt,kMaxAttempts);
    }
    if(attempt<kMaxAttempts) std::this_thread::sleep_for(std::chrono::milliseconds(200*attempt));
  }
  return false;
#endif
}

bool R2Client::verify(const std::string& key) const {
  if(!enabled()) return false;
  if(cfg.bucket.empty()) return false;
  const char* tm=std::getenv("EXAMVAN_R2_TESTMODE");
  if(tm && std::string(tm)=="1") return true;
#ifndef HAS_LIBCURL
  return false;
#else
  if(cfg.endpoint.find("r2.cloudflarestorage.com")==std::string::npos) return false;
  std::string payload_hash=sha256_hex(std::string{});
  std::string host, canonical_uri;
  std::string url=build_url(cfg, key, host, canonical_uri);
  std::string auth, date_full, date8;
  curl_append_header(auth, date_full, date8, cfg, "HEAD", canonical_uri, payload_hash, host);
  CURL* c=curl_easy_init();
  if(!c) return false;
  struct curl_slist* hdrs=nullptr;
  hdrs=curl_slist_append(hdrs, ("Authorization: "+auth).c_str());
  hdrs=curl_slist_append(hdrs, ("x-amz-date: "+date_full).c_str());
  hdrs=curl_slist_append(hdrs, ("x-amz-content-sha256: "+payload_hash).c_str());
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "HEAD");
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_discard_cb);
  curl_easy_setopt(c, CURLOPT_NOBODY, 0L);
  CURLcode rc=curl_easy_perform(c);
  long code=0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);
  if(rc!=CURLE_OK) return false;
  return code>=200 && code<300;
#endif
}

bool R2Client::remove(const std::string& key) const {
  if(!enabled()) return false;
  if(cfg.bucket.empty()) return false;
  const char* tm=std::getenv("EXAMVAN_R2_TESTMODE");
  if(tm && std::string(tm)=="1") return true;
#ifndef HAS_LIBCURL
  return false;
#else
  if(cfg.endpoint.find("r2.cloudflarestorage.com")==std::string::npos) return false;
  std::string payload_hash=sha256_hex(std::string{});
  std::string host, canonical_uri;
  std::string url=build_url(cfg, key, host, canonical_uri);
  std::string auth, date_full, date8;
  curl_append_header(auth, date_full, date8, cfg, "DELETE", canonical_uri, payload_hash, host);
  CURL* c=curl_easy_init();
  if(!c) return false;
  struct curl_slist* hdrs=nullptr;
  hdrs=curl_slist_append(hdrs, ("Authorization: "+auth).c_str());
  hdrs=curl_slist_append(hdrs, ("x-amz-date: "+date_full).c_str());
  hdrs=curl_slist_append(hdrs, ("x-amz-content-sha256: "+payload_hash).c_str());
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_discard_cb);
  curl_easy_setopt(c, CURLOPT_NOBODY, 0L);
  CURLcode rc=curl_easy_perform(c);
  long code=0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);
  if(rc!=CURLE_OK) return false;
  return code>=200 && code<300;
#endif
}

} // namespace examvan::r2
