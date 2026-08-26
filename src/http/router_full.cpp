#include "http/router_full.hpp"
#include "http/handlers.hpp"
#include "handlers/public/hasil.hpp"
#include "handlers/public/download.hpp"
#include "handlers/api/exams.hpp"
#include "handlers/api/webhook.hpp"
#include "handlers/admin/dashboard.hpp"
#include "handlers/admin/users.hpp"
#include "handlers/admin/vouchers.hpp"
#include "handlers/admin/exams.hpp"
#include "handlers/admin/settings.hpp"
#include "handlers/admin/pengawas.hpp"
#include "handlers/admin/submissions.hpp"
#include "handlers/auth/login.hpp"
#include "handlers/auth/logout.hpp"
#include "handlers/public/template_helper.hpp"
#include "session/cookie.hpp"
#include <fstream>
#include <sstream>

namespace examvan {

void register_full_routes(Router& r, const Config& cfg){
  register_routes(r, cfg);

  r.add("GET","/login", [cfg](const Request& req){ return handlers::auth::login_page(req); });
  r.add("POST","/login", [cfg](const Request& req){ return handlers::auth::login_handler(req, cfg); });
  r.add("POST","/logout", [](const Request& req){ return handlers::auth::logout_handler(req); });
  r.add("GET","/logout", [](const Request& req){ return handlers::auth::logout_page(req); });
  r.add("GET","/register", [](const Request&){
    std::string html=handlers::public_::render_public_template("register","2.7.2");
    Response res; res.status=200; res.headers["Content-Type"]="text/html";
    res.body=html.empty()?"<html>Register</html>":html; return res;
  });
  r.add("POST","/register", [](const Request&){ Response res; res.json(200,"{\"ok\":true}"); return res; });
  r.add("GET","/register/confirm", [](const Request&){
    std::string html=handlers::public_::render_public_template("register_confirm","2.7.2");
    Response res; res.status=200; res.headers["Content-Type"]="text/html";
    res.body=html.empty()?"<html>Confirm</html>":html; return res;
  });
  r.add("POST","/register/confirm", [](const Request&){ Response res; res.json(200,"{\"ok\":true}"); return res; });
  r.add("POST","/register/resend", [](const Request&){ Response res; res.json(200,"{\"ok\":true}"); return res; });
  r.add("GET","/forgot-password", [](const Request&){
    std::string html=handlers::public_::render_public_template("forgot_password","2.7.2");
    Response res; res.status=200; res.headers["Content-Type"]="text/html";
    res.body=html.empty()?"<html>Forgot</html>":html; return res;
  });
  r.add("POST","/forgot-password", [](const Request&){ Response res; res.json(200,"{\"ok\":true}"); return res; });
  r.add("GET","/reset-password", [](const Request&){
    std::string html=handlers::public_::render_public_template("reset_password","2.7.2");
    Response res; res.status=200; res.headers["Content-Type"]="text/html";
    res.body=html.empty()?"<html>Reset</html>":html; return res;
  });
  r.add("POST","/reset-password", [](const Request&){ Response res; res.json(200,"{\"ok\":true}"); return res; });
  r.add("GET","/download", handlers::public_::download_page);
  r.add("GET","/download/apk", handlers::public_::download_apk);
  r.add("GET","/download/app/:id", handlers::public_::download_system_app);
  r.add("GET","/hasil", handlers::public_::cek_hasil_page);
  r.add("GET","/hasil/:token", handlers::public_::hasil_page);
  r.add("GET","/:token", [](const Request& req){
    auto it=req.params.find("token"); std::string t=it!=req.params.end()?it->second:"";
    Response res; res.status=302; res.headers["Location"]="/hasil/"+t; return res;
  });

  r.add("GET","/api/health", handlers::api::health);
  r.add("GET","/api/time", handlers::api::time_handler);
  r.add("GET","/api/exams", handlers::api::list_exams);
  r.add("POST","/api/exams/request-approval", handlers::api::request_approval);
  r.add("GET","/api/exams/token/:token", handlers::api::exam_by_token);
  r.add("GET","/api/exams/:exam_id/pdf", handlers::api::exam_pdf);
  r.add("POST","/api/exams/:exam_id/submit", handlers::api::submit_exam);
  r.add("GET","/api/exams/:exam_id/result", handlers::api::exam_result);
  r.add("POST","/api/exams/:exam_id/access-log", handlers::api::access_log);
  r.add("POST","/api/exams/:exam_id/complete", handlers::api::complete_exam);
  r.add("GET","/api/hasil/:token", handlers::public_::cek_hasil_api);
  r.add("POST","/api/webhook", handlers::api::webhook);

  r.add("GET","/admin", [](const Request&){ Response rr; rr.status=302; rr.headers["Location"]="/admin/dashboard"; return rr; });
  r.add("GET","/admin/dashboard", [cfg](const Request& req){
    auto it=req.headers.find("Cookie");
    if(it==req.headers.end() || !verify_session_cookie(cfg.secret_key, it->second).has_value()){
      Response rr; rr.status=302; rr.headers["Location"]="/login?next=/admin/dashboard"; return rr;
    }
    return handlers::admin::dashboard_page(req);
  });
  r.add("GET","/admin/settings", [cfg](const Request& req){
    auto it=req.headers.find("Cookie");
    if(it==req.headers.end() || !verify_session_cookie(cfg.secret_key, it->second).has_value()){
      Response rr; rr.status=302; rr.headers["Location"]="/login?next=/admin/settings"; return rr;
    }
    return handlers::admin::settings_page(req);
  });
  r.add("GET","/admin/api/stats", handlers::admin::dashboard_stats);
  r.add("GET","/admin/api/saas-settings", handlers::admin::settings_page);
  r.add("POST","/admin/api/saas-settings", handlers::admin::update_settings);
  r.add("GET","/admin/api/users", handlers::admin::list_users);
  r.add("GET","/admin/api/users/:id", handlers::admin::list_users);
  r.add("POST","/admin/api/users", handlers::admin::create_user);
  r.add("PUT","/admin/api/users/:id", handlers::admin::edit_user);
  r.add("DELETE","/admin/api/users/:id", handlers::admin::delete_user);
  r.add("POST","/admin/api/instansi/update", handlers::admin::instansi_update);
  r.add("POST","/admin/api/change-password", [](const Request&){ Response rr; rr.json(200,"{\"ok\":true}"); return rr; });
  r.add("GET","/admin/api/vouchers", handlers::admin::list_vouchers);
  r.add("GET","/admin/api/vouchers/mine", handlers::admin::list_vouchers);
  r.add("POST","/admin/api/vouchers/redeem", handlers::admin::redeem_voucher);
  r.add("POST","/admin/api/vouchers/activate", handlers::admin::activate_voucher);
  r.add("GET","/admin/api/vouchers/audit-logs", handlers::admin::list_vouchers);
  r.add("GET","/admin/api/packages", handlers::admin::list_vouchers);
  r.add("GET","/admin/api/saas-settings", handlers::admin::settings_page);
  r.add("GET","/admin/api/exams", handlers::admin::list_admin_exams);
  r.add("POST","/admin/api/exams", handlers::admin::create_exam);
  r.add("POST","/admin/api/upload", handlers::admin::create_exam);
  r.add("PUT","/admin/api/exams/:id", handlers::admin::update_exam);
  r.add("DELETE","/admin/api/exams/:id", handlers::admin::delete_exam);
  r.add("POST","/admin/api/exams/:exam_id/toggle", handlers::admin::update_exam);
  r.add("POST","/admin/api/exams/:exam_id/delete", handlers::admin::delete_exam);
  r.add("POST","/admin/api/exams/:exam_id/edit", handlers::admin::update_exam);
  r.add("POST","/admin/api/exams/:exam_id/start", handlers::admin::update_exam);
  r.add("POST","/admin/api/exams/:exam_id/stop", handlers::admin::update_exam);
  r.add("POST","/admin/api/exams/:exam_id/regenerate-token", handlers::admin::update_exam);
  r.add("GET","/admin/api/exams/:id/export", handlers::admin::export_xlsx);
  r.add("GET","/admin/api/submissions", handlers::admin::list_submissions);
  r.add("GET","/admin/api/submissions/:id/detail", handlers::admin::submission_detail);
  r.add("GET","/admin/api/submissions/export", handlers::admin::export_xlsx);
  r.add("GET","/admin/api/queue/status", handlers::admin::queue_status);
  r.add("POST","/admin/api/submissions/:id/delete", handlers::admin::delete_submission);
  r.add("GET","/admin/api/pengawas/exams", handlers::admin::pengawas_exams);
  r.add("GET","/admin/api/pengawas/exams/:exam_id/submissions", handlers::admin::pengawas_submissions);
  r.add("GET","/admin/api/pengawas/exams/:exam_id/approvals", handlers::admin::pending_approvals);
  r.add("POST","/admin/api/pengawas/exams/:exam_id/approvals/:mac_address", handlers::admin::set_approval);
  r.add("GET","/admin/api/pengawas/exams/:exam_id/auto-approve", handlers::admin::get_auto_approve);
  r.add("POST","/admin/api/pengawas/exams/:exam_id/auto-approve", handlers::admin::set_auto_approve);
  r.add("GET","/admin/api/system-apps", handlers::admin::settings_page);
  r.add("POST","/admin/api/system-apps", handlers::admin::update_settings);
}

} // namespace examvan
