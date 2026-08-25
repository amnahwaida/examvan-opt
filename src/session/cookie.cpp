#include "session/cookie.hpp"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <sstream>
#include <algorithm>

namespace examvan {

std::string b64_encode(const std::string& s) {
  int len = 4 * ((static_cast<int>(s.size()) + 2) / 3);
  std::string out(len, '\0');
  int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                          reinterpret_cast<const unsigned char*>(s.data()), static_cast<int>(s.size()));
  out.resize(n);
  return out;
}

std::string b64_decode(const std::string& s) {
  std::string in = s;
  while (in.size() % 4) in += '=';
  std::string out(in.size(), '\0');
  int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                          reinterpret_cast<const unsigned char*>(in.data()), static_cast<int>(in.size()));
  if (n < 0) return "";
  size_t pad = 0;
  if (s.size() >= 2 && s.substr(s.size()-2)=="==") pad=2;
  else if (!s.empty() && s.back()=='=') pad=1;
  else {
    size_t eq = std::count(in.begin(), in.end(), '=');
    pad = eq;
  }
  out.resize(n - pad);
  while (!out.empty() && out.back()=='\0') out.pop_back();
  return out;
}

std::string b64url_encode(const std::string& s){
  std::string b=b64_encode(s);
  for(char& c: b){ if(c=='+') c='-'; else if(c=='/') c='_'; }
  b.erase(std::remove(b.begin(), b.end(), '='), b.end());
  return b;
}

std::string b64url_decode(const std::string& s){
  std::string t=s;
  for(char& c: t){ if(c=='-') c='+'; else if(c=='_') c='/'; }
  while(t.size()%4) t+='=';
  return b64_decode(t);
}

std::string hmac_sha256_b64(const std::string& key, const std::string& data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char*>(data.data()), data.size(), md, &md_len);
  std::string raw(reinterpret_cast<char*>(md), md_len);
  return b64_encode(raw);
}

std::string encode_cookie_value(const std::string& secret, const std::string& payload_b64) {
  std::string sig = hmac_sha256_b64(secret, payload_b64);
  return payload_b64 + "." + sig;
}

std::optional<std::string> decode_cookie_value(const std::string& secret, const std::string& cookie_value) {
  auto dot = cookie_value.rfind('.');
  if (dot == std::string::npos) return std::nullopt;
  std::string payload = cookie_value.substr(0, dot);
  std::string sig = cookie_value.substr(dot+1);
  std::string expected = hmac_sha256_b64(secret, payload);
  if (expected != sig) return std::nullopt;
  return b64_decode(payload);
}

std::optional<std::string> decode_cookie_value_dual(const std::string& cur, const std::string& prev, const std::string& val){
  auto r=decode_cookie_value(cur, val);
  if(r) return r;
  if(!prev.empty()) return decode_cookie_value(prev, val);
  return std::nullopt;
}

bool is_securecookie_format(const std::string& val){
  return val.find('|')!=std::string::npos || (val.size()>20 && val.find('.')!=std::string::npos);
}

std::string extract_cookie(const std::string& header, const std::string& name) {
  std::string needle = name + "=";
  size_t pos = 0;
  while (true) {
    size_t found = header.find(needle, pos);
    if (found == std::string::npos) return "";
    if (found==0 || header[found-1]==';' || header[found-1]==' ' || header[found-1]==',') {
      size_t start = found + needle.size();
      size_t end = header.find(';', start);
      std::string val = header.substr(start, end==std::string::npos? std::string::npos : end-start);
      size_t s = val.find_first_not_of(" \t");
      size_t e = val.find_last_not_of(" \t");
      if (s==std::string::npos) return "";
      return val.substr(s, e-s+1);
    }
    pos = found+1;
  }
}

static std::map<std::string,std::string> parse_kv(const std::string& s) {
  std::map<std::string,std::string> m;
  std::istringstream ss(s);
  std::string pair;
  while (std::getline(ss, pair, '&')) {
    auto eq = pair.find('=');
    if (eq==std::string::npos) continue;
    m[pair.substr(0,eq)] = pair.substr(eq+1);
  }
  return m;
}

std::optional<SessionData> verify_session_cookie(const std::string& secret, const std::string& cookie_header_value) {
  std::string cookie_val = extract_cookie(cookie_header_value, "examvan_session");
  if (cookie_val.empty()) cookie_val = cookie_header_value;
  auto decoded = decode_cookie_value(secret, cookie_val);
  if (!decoded){
    std::string url_dec = b64url_decode(cookie_val);
    if(!url_dec.empty()) decoded = decode_cookie_value(secret, url_dec);
  }
  if (!decoded) return std::nullopt;
  SessionData d;
  d.fields = parse_kv(*decoded);
  auto it = d.fields.find("admin_id");
  if (it!=d.fields.end()) try{d.admin_id=std::stoi(it->second);}catch(...){}
  it = d.fields.find("username"); if(it!=d.fields.end()) d.username=it->second;
  it = d.fields.find("role"); if(it!=d.fields.end()) d.role=it->second;
  it = d.fields.find("instansi"); if(it!=d.fields.end()) d.instansi=it->second;
  it = d.fields.find("is_super_admin"); if(it!=d.fields.end()) d.is_super_admin=it->second=="1"||it->second=="true";
  return d;
}

std::optional<SessionData> verify_session_cookie_dual(const std::string& cur, const std::string& prev, const std::string& hdr){
  auto r=verify_session_cookie(cur, hdr);
  if(r) return r;
  if(!prev.empty()) return verify_session_cookie(prev, hdr);
  return std::nullopt;
}

}  // namespace examvan
