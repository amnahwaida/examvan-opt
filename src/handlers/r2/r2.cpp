#include "handlers/r2/r2.hpp"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <ctime>

namespace examvan::r2 {

std::string presign_url(const R2Config& cfg, const std::string& key, int expires_seconds){
  if(!cfg.enabled()) return "";
  std::time_t now=time(nullptr);
  char date[32]; std::strftime(date,sizeof(date),"%Y%m%dT%H%M%SZ", std::gmtime(&now));
  std::string credential = cfg.access_key + "/" + std::string(date,8) + "/auto/s3/aws4_request";
  std::ostringstream ss;
  ss<< cfg.endpoint << "/" << cfg.bucket << "/" << key
    << "?X-Amz-Algorithm=AWS4-HMAC-SHA256"
    << "&X-Amz-Credential=" << credential
    << "&X-Amz-Date=" << date
    << "&X-Amz-Expires=" << expires_seconds
    << "&X-Amz-SignedHeaders=host";
  std::string to_sign = "GET\n/"+cfg.bucket+"/"+key+"\n"+ss.str();
  unsigned char md[EVP_MAX_MD_SIZE]; unsigned int md_len=0;
  HMAC(EVP_sha256(), cfg.secret_key.data(), cfg.secret_key.size(),
       reinterpret_cast<const unsigned char*>(to_sign.data()), to_sign.size(), md, &md_len);
  std::ostringstream sig; for(unsigned i=0;i<md_len;i++) sig<<std::hex<<std::setw(2)<<std::setfill('0')<<(int)md[i];
  ss<< "&X-Amz-Signature=" << sig.str();
  return ss.str();
}

std::string object_key_for_exam(int exam_id, const std::string& filename){
  return "exams/" + std::to_string(exam_id) + "/" + filename;
}

std::string object_key_for_app(const std::string& version, const std::string& flavor){
  return "apps/android/" + version + "/EXAMVAN-v" + version + "-" + flavor + ".apk";
}

} // namespace examvan::r2
