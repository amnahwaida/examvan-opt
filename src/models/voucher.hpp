#pragma once
#include <string>
#include <optional>

namespace examvan::models {

struct Voucher {
  int id{0};
  std::string code;
  std::string package_name;
  int max_uses{1};
  int used_count{0};
  std::string status{"active"};
  std::optional<std::string> expires_at;
  std::string created_at;
};

struct SystemApp {
  int id{0};
  std::string flavor;
  std::string version;
  int64_t size_bytes{0};
  std::string file_path;
  std::string created_at;
};

} // namespace examvan::models
