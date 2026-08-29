#include "config/config.hpp"
#include <cstdlib>
#include <stdexcept>

namespace examvan {

int env_int(const char* key, int def) {
  if (const char* v = std::getenv(key)) {
    try { return std::stoi(v); } catch (...) { return def; }
  }
  return def;
}

std::string env_str(const char* key, const std::string& def) {
  if (const char* v = std::getenv(key)) return std::string(v);
  return def;
}

bool Config::is_development() const {
  const char* e = std::getenv("APP_ENV");
  return e && std::string(e) == "development";
}

Config Config::load() {
  Config c;
  c.port = env_int("PORT", 5000);
  if (const char* v = std::getenv("DATABASE_URL")) c.database_url = v;
  if (const char* v = std::getenv("REDIS_URL")) c.redis_url = v;
  if (const char* v = std::getenv("EXAMVAN_SECRET")) c.secret_key = v;
  if (const char* v = std::getenv("EXAMVAN_ADMIN_USER")) c.admin_user = v;
  if (const char* v = std::getenv("EXAMVAN_ADMIN_PASS")) c.admin_pass = v;
  c.storage_path = env_str("STORAGE_PATH", "/app/storage");
  c.version = env_str("EXAMVAN_VERSION", c.version);
  c.database_max_conns = env_int("DATABASE_MAX_CONNS", 60);
  if (const char* v = std::getenv("R2_ACCESS_KEY_ID")) c.r2_access_key = v;
  if (const char* v = std::getenv("R2_SECRET_ACCESS_KEY")) c.r2_secret_key = v;
  c.r2_bucket = env_str("R2_BUCKET", "examvan-pdfs");
  if (const char* v = std::getenv("R2_ENDPOINT")) c.r2_endpoint = v;
  if (const char* v = std::getenv("EXAMVAN_CORS_ORIGINS")) c.cors_origins = v;
  return c;
}

void Config::validate() const {
  if (secret_key.empty()) throw std::runtime_error("EXAMVAN_SECRET required");
  if (secret_key.size() < 32) throw std::runtime_error("EXAMVAN_SECRET must be at least 32 characters");
  if (admin_user.empty()) throw std::runtime_error("EXAMVAN_ADMIN_USER required");
  if (admin_pass.empty()) throw std::runtime_error("EXAMVAN_ADMIN_PASS required");
  if (r2_access_key.empty() || r2_secret_key.empty() || r2_endpoint.empty())
    throw std::runtime_error("R2 credentials required (R2_ACCESS_KEY_ID etc)");
  if (port < 1 || port > 65535) throw std::runtime_error("PORT out of range");
  if (database_max_conns < 1 || database_max_conns > 150) throw std::runtime_error("DATABASE_MAX_CONNS out of range");
  if(!database_url.empty()){
    if(database_url.rfind("postgresql://",0)!=0 && database_url.rfind("postgres://",0)!=0) throw std::runtime_error("DATABASE_URL must be postgresql:// or postgres://");
  }
}

}  // namespace examvan
