#pragma once
#include <string>

namespace examvan {

struct Config {
  int port{5000};
  std::string database_url;
  std::string redis_url;
  std::string secret_key;
  std::string secret_prev;
  std::string turnstile_secret;
  std::string admin_user;
  std::string admin_pass;
  std::string storage_path{"/app/storage"};
  int64_t max_file_size{5 * 1024 * 1024};
  std::string version{"2.7.2"};
  int database_max_conns{60};
  std::string r2_access_key;
  std::string r2_secret_key;
  std::string r2_bucket{"examvan-pdfs"};
  std::string r2_endpoint;
  std::string cors_origins;
  bool protobuf_mandatory{false};
  bool is_development() const;

  static Config load();
  void validate() const;
};

int env_int(const char* key, int def);
std::string env_str(const char* key, const std::string& def);

}  // namespace examvan
