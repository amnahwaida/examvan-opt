#include "handlers/r2/r2.hpp"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <ctime>

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
static std::string sha256_hex(const std::string& s){
  unsigned char h[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), h);
  std::ostringstream o; for(int i=0;i<SHA256_DIGEST_LENGTH;i++) o<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)h[i];
  return o.str();
}
std::string presign_url(const R2Config& cfg, const std::string& key, int expires_seconds){
  if(!cfg.enabled()) return "";
  if(cfg.bucket.empty()) return "";
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
  std::string credential_raw = cfg.access_key + "/" + date8 + "/auto/s3/aws4_request";
  std::string credential = uri_encode(credential_raw);
  std::string host = cfg.endpoint;
  auto p = host.find("://");
  if(p!=std::string::npos) host = host.substr(p+3);
  host = host.substr(0, host.find('/'));
  std::string canonical_uri = "/" + cfg.bucket + "/" + key;
  std::string canonical_qs = "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=" + credential + "&X-Amz-Date=" + std::string(date_full) + "&X-Amz-Expires=" + std::to_string(expires_seconds) + "&X-Amz-SignedHeaders=host";
  std::string canonical_headers = "host:" + host + "\n";
  std::string signed_headers = "host";
  std::string payload_hash = "UNSIGNED-PAYLOAD";
  std::string canonical_request = "GET\n" + canonical_uri + "\n" + canonical_qs + "\n" + canonical_headers + "\n" + signed_headers + "\n" + payload_hash;
  std::string hashed_canonical = sha256_hex(canonical_request);
  std::string string_to_sign = "AWS4-HMAC-SHA256\n" + std::string(date_full) + "\n" + date8 + "/auto/s3/aws4_request\n" + hashed_canonical;
  auto hmac = [](const std::string& k, const std::string& d){ unsigned char md[EVP_MAX_MD_SIZE]; unsigned int l=0; HMAC(EVP_sha256(), k.data(), k.size(), reinterpret_cast<const unsigned char*>(d.data()), d.size(), md, &l); return std::string(reinterpret_cast<char*>(md), l); };
  std::string kDate = hmac("AWS4"+cfg.secret_key, date8);
  std::string kRegion = hmac(kDate, "auto");
  std::string kService = hmac(kRegion, "s3");
  std::string kSigning = hmac(kService, "aws4_request");
  unsigned char sig_md[EVP_MAX_MD_SIZE]; unsigned int sig_len=0;
  HMAC(EVP_sha256(), kSigning.data(), kSigning.size(), reinterpret_cast<const unsigned char*>(string_to_sign.data()), string_to_sign.size(), sig_md, &sig_len);
  std::ostringstream sig; for(unsigned i=0;i<sig_len;i++) sig<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)sig_md[i];
  std::ostringstream ss;
  ss<< cfg.endpoint << "/" << cfg.bucket << "/" << key
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

} // namespace examvan::r2
