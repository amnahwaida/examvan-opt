#pragma once
#include <string>
#include <optional>

namespace examvan::r2 {

inline constexpr const char* kErrNotConfigured = "Cloudflare R2 tidak dikonfigurasi.";
inline constexpr const char* kErrUploadFailed = "Gagal mengupload file ke Cloudflare R2";
inline constexpr const char* kErrSignFailed = "Gagal menghasilkan URL unduhan.";
inline constexpr const char* kCodeNotConfigured = "R2_NOT_CONFIGURED";
inline constexpr const char* kCodeUploadFailed = "UPLOAD_FAILED";
inline constexpr const char* kCodeSignFailed = "SIGNED_URL_FAILED";

struct R2Config {
  std::string access_key;
  std::string secret_key;
  std::string endpoint;
  std::string bucket;
  bool enabled() const { return !access_key.empty() && !secret_key.empty() && !endpoint.empty(); }
};

std::string presign_url(const R2Config& cfg, const std::string& key, int expires_seconds = 3600);
std::string object_key_for_exam(int exam_id, const std::string& filename);
std::string object_key_for_app(const std::string& version, const std::string& flavor);

struct R2Client {
  R2Config cfg;
  bool enabled() const { return cfg.enabled(); }
  std::string signed_url(const std::string& key, int ttl=3600) const { return presign_url(cfg,key,ttl); }
  bool upload(const std::string& key, const std::string& data) const { (void)key;(void)data; return enabled(); }
  bool remove(const std::string& key) const { (void)key; return enabled(); }
};

} // namespace examvan::r2
