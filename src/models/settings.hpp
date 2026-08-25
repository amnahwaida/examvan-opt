#pragma once
#include <string>
#include <map>
#include <optional>

namespace examvan::models {

struct SaasSetting { std::string key; std::string value; };

inline const char* kDefaultActiveDays = "default_active_days";
inline const char* kAndroidVersion = "android_version";
inline const char* kAccessLogRetentionDays = "access_log_retention_days";

inline std::map<std::string,std::string> default_settings(){
  return {
    {kDefaultActiveDays, "14"},
    {kAndroidVersion, "2.7.2"},
    {kAccessLogRetentionDays, "30"},
    {"turnstile_enabled", "0"},
    {"auto_cleanup_days", "7"},
  };
}

inline std::optional<std::string> get_setting(const std::map<std::string,std::string>& m, const std::string& k){
  auto it=m.find(k); if(it==m.end()) return std::nullopt; return it->second;
}

} // namespace examvan::models
