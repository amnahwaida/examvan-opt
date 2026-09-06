#pragma once
#include <string>
#include <optional>
#include <vector>
#include <regex>
#include <vector>

namespace examvan::models {

inline const std::string kUserStatusActive = "active";
inline const std::string kUserStatusSuspended = "suspended";
inline const std::string kUserStatusPendingOTP = "pending_otp";
inline const std::string kRoleGuru = "guru";
inline const std::string kRolePengawas = "pengawas";
inline const std::string kRoleOperator = "operator";
inline const std::string kRoleSuperAdmin = "superadmin";

struct AdminUser {
  int id{0};
  std::string username;
  std::string name;
  std::string password_hash;
  std::string status{kUserStatusActive};
  std::string instansi;
  std::optional<int> instansi_id;
  std::optional<std::string> instansi_code;
  std::string role{"[\"guru\"]"};
  int max_exams{3};
  int64_t max_pdf_size{1048576};
  int max_concurrent_exams{2};
  int64_t max_storage_size{52428800};
  std::string whatsapp_number;
  std::string email;
  std::optional<std::string> expires_at;
  bool operator_created{false};
  std::optional<int> created_by;
  bool suspended_by_cascade{false};
};

inline bool is_valid_username(const std::string& s) {
  static const std::regex re(R"(^[a-z0-9._-]{3,32}$)");
  return std::regex_match(s, re);
}

inline bool has_role(const std::string& role_json, const std::string& role) {
  if(role_json.find("\"" + role + "\"") != std::string::npos) return true;
  // Accept legacy test/dev cookies that encoded a single role without JSON
  // quotes, while still requiring an exact token rather than substring match.
  return role_json==("["+role+"]") || role_json==role;
}

inline std::vector<std::string> parse_roles(const std::string& role_json){
  std::vector<std::string> out;
  for(const std::string& role: {kRoleGuru,kRolePengawas,kRoleOperator,kRoleSuperAdmin}){
    if(has_role(role_json,role)) out.push_back(role);
  }
  return out;
}

inline bool is_super_admin_role(const std::string& role_json){
  return has_role(role_json,kRoleSuperAdmin);
}

inline std::string normalize_role(const std::string& r) {
  auto roles=parse_roles(r);
  if (roles.empty()) return "[\"guru\"]";
  std::string out="[";
  for(size_t i=0;i<roles.size();++i){ if(i) out+=","; out+="\""+roles[i]+"\""; }
  return out+"]";
}

inline bool is_feature_locked(const AdminUser& u, const std::string& super_admin_user) {
  if (u.username == super_admin_user) return false;
  if (has_role(u.role, kRoleSuperAdmin)) return false;
  if (!u.expires_at.has_value() || u.expires_at->empty()) return false;
  return true;
}

inline const char* kUserColumns = "id, username, name, password_hash, status, instansi, instansi_id, role, max_exams, max_pdf_size, max_concurrent_exams, max_storage_size, whatsapp_number, email, expires_at, operator_created, created_by";

} // namespace examvan::models
