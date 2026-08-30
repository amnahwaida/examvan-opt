#include "handlers/admin/settings.hpp"
#include "handlers/admin/template_helper.hpp"
namespace examvan::handlers::admin {
Response settings_page(const Request& req){
  if(req.path.find("/api/")!=std::string::npos){
    if(req.path.find("system-apps")!=std::string::npos){
      Response r; r.json(200,"{\"success\":true,\"apps\":[],\"system_apps\":[]}"); return r;
    }
    Response r; r.json(200,"{\"success\":true,\"settings\":{\"email_verification_enabled\":false,\"smtp_host\":\"smtp.gmail.com\",\"smtp_port\":587,\"smtp_user\":\"\",\"smtp_password\":\"\",\"smtp_sender_name\":\"EXAMVAN\",\"default_max_exams\":3,\"default_max_concurrent_exams\":2,\"default_max_pdf_size_mb\":1,\"default_max_storage_size_mb\":50,\"default_active_days\":14,\"android_version\":\"2.1.9\",\"webapp_version\":\"2.1.9\",\"seo_title\":\"\",\"seo_description\":\"\",\"seo_keywords\":\"\",\"seo_index\":false,\"footer_text\":\"© 2026 EXAMVAN Team. All rights reserved.\",\"footer_tagline\":\"\",\"voucher_redeem_enabled\":true,\"turnstile_enabled\":false,\"turnstile_site_key\":\"\",\"turnstile_secret_key\":\"\",\"max_accounts_per_ip\":3,\"max_approvals_per_exam\":500,\"approval_cleanup_interval_minutes\":15,\"approval_cleanup_ended_grace_hours\":1,\"approval_cleanup_inactive_ttl_hours\":24,\"storage_free_mb\":10240}}"); return r;
  }
  std::string html=render_admin_template("settings","2.7.2");
  if(!html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Settings</h1></body></html>"; return r;
}
Response update_settings(const Request&){
  Response r; r.json(200,"{\"success\":true}"); return r;
}
Response system_apps_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>System Apps</h1></body></html>"; return r;
}
} // namespace examvan::handlers::admin
