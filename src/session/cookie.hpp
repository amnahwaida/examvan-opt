#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace examvan {

struct SessionData {
  std::map<std::string, std::string> fields;
  bool is_super_admin{false};
  std::string username;
  std::string role;
  std::string instansi;
  int admin_id{0};
};

std::string b64_encode(const std::string& s);
std::string b64_decode(const std::string& s);
std::string b64url_encode(const std::string& s);
std::string b64url_decode(const std::string& s);
std::string hmac_sha256_b64(const std::string& key, const std::string& data);

std::string encode_cookie_value(const std::string& secret, const std::string& payload_b64);
std::optional<std::string> decode_cookie_value(const std::string& secret, const std::string& cookie_value);
std::optional<std::string> decode_cookie_value_dual(const std::string& current_secret, const std::string& previous_secret, const std::string& cookie_value);

std::optional<SessionData> verify_session_cookie(const std::string& secret, const std::string& cookie_header_value);
std::optional<SessionData> verify_session_cookie_dual(const std::string& current_secret, const std::string& previous_secret, const std::string& cookie_header_value);
std::string extract_cookie(const std::string& cookie_header, const std::string& name);

bool is_securecookie_format(const std::string& val);

}  // namespace examvan
