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
#include "middleware/auth.hpp"
#include "handlers/auth/logout.hpp"
#include "handlers/public/template_helper.hpp"
#include "session/cookie.hpp"
#include "session/csrf.hpp"
#include "middleware/ratelimit.hpp"
#include "middleware/body_limit.hpp"
#include "middleware/cors.hpp"
#include <fstream>
#include <sstream>

namespace examvan {

void register_full_routes(Router& r, const Config& cfg){
  (void)middleware::is_origin_allowed("", cfg.cors_origins);
  register_routes(r, cfg);

  /* Guard sesi untuk SEMUA route /admin/api: tanpa cookie examvan_session yang valid,
   * handler tidak dieksekusi → 401 JSON (format dipahami apiFetch admin-core.js:
   * event auth:expired + redirect /admin/login?next=). */
  static middleware::RateLimiter g_admin_rl(100, std::chrono::seconds(60));
  auto admin_api=[cfg](Handler h)->Handler{
    return [cfg,h](const Request& req)->Response{
      if(req.body.size()>5*1024*1024){ Response rr; rr.status=413; rr.body="payload too large"; return rr; }
      std::string ip="global";
      auto it_ip=req.headers.find("X-Real-IP");
      if(it_ip!=req.headers.end()) ip=it_ip->second;
      if(!g_admin_rl.allow(ip)){ Response rr; rr.status=429; rr.json(429,"{\"error\":\"rate limit exceeded\"}"); return rr; }
      auto _bl = middleware::body_limit(req, 5*1024*1024, [&](const Request& r){ return h(r); });
      if(_bl.status==413) return _bl;
      std::string key=cfg.secret_key;
      std::string prev=cfg.secret_prev;
      auto it=req.headers.find("Cookie");
      bool ok=false;
      if(it!=req.headers.end()){
        if(prev.empty()) ok=verify_session_cookie(key,it->second).has_value();
        else ok=verify_session_cookie_dual(key,prev,it->second).has_value();
        if(!ok) ok=middleware::is_authenticated(req,key);
      }
      if(!ok){
        Response rr; rr.status=401; rr.json(401,"{\"success\":false,\"message\":\"unauthorized\"}"); return rr;
      }
      return h(req);
    };
  };

  r.add("GET","/login", [cfg](const Request& req){ return handlers::auth::login_page(req); });
  r.add("POST","/login", [cfg](const Request& req){ return handlers::auth::login_handler(req, cfg); });
  /* Alias /admin/login: template login & admin-core.js memakai path ini.
   * Tanpa alias, submit form login → fallback 404 JSON {"error":"not found"}. */
  r.add("GET","/admin/login", [cfg](const Request& req){ return handlers::auth::login_page(req); });
  r.add("POST","/admin/login", [cfg](const Request& req){ return handlers::auth::login_handler(req, cfg); });
  r.add("POST","/logout", [](const Request& req){ return handlers::auth::logout_handler(req); });
  r.add("GET","/logout", [](const Request& req){ return handlers::auth::logout_page(req); });
  r.add("GET","/register", [](const Request& req){
    std::string csrf=generate_csrf_token();
    std::string html=handlers::public_::render_public_template("register","2.7.2");
    size_t p=html.find("CSRF_PLACEHOLDER");
    while(p!=std::string::npos){ html.replace(p,16,csrf); p=html.find("CSRF_PLACEHOLDER",p+csrf.size()); }
    Response res; res.status=200; res.headers["Content-Type"]="text/html";
    std::string ck="csrf_token="+csrf+"; Path=/; HttpOnly; SameSite=Lax"; if(!Config::load().is_development()) ck+="; Secure";
    res.headers["Set-Cookie"]=ck;
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
  r.add("GET","/forgot-password", [](const Request& req){
    std::string csrf=generate_csrf_token();
    std::string html=handlers::public_::render_public_template("forgot_password","2.7.2");
    size_t p=html.find("CSRF_PLACEHOLDER");
    while(p!=std::string::npos){ html.replace(p,16,csrf); p=html.find("CSRF_PLACEHOLDER",p+csrf.size()); }
    Response res; res.status=200; res.headers["Content-Type"]="text/html";
    std::string ck="csrf_token="+csrf+"; Path=/; HttpOnly; SameSite=Lax"; if(!Config::load().is_development()) ck+="; Secure";
    res.headers["Set-Cookie"]=ck;
    res.body=html.empty()?"<html>Forgot</html>":html; return res;
  });
  r.add("POST","/forgot-password", [](const Request&){ Response res; res.json(200,"{\"ok\":true}"); return res; });
  r.add("GET","/reset-password", [](const Request& req){
    std::string csrf=generate_csrf_token();
    std::string html=handlers::public_::render_public_template("reset_password","2.7.2");
    size_t p=html.find("CSRF_PLACEHOLDER");
    while(p!=std::string::npos){ html.replace(p,16,csrf); p=html.find("CSRF_PLACEHOLDER",p+csrf.size()); }
    Response res; res.status=200; res.headers["Content-Type"]="text/html";
    std::string ck="csrf_token="+csrf+"; Path=/; HttpOnly; SameSite=Lax"; if(!Config::load().is_development()) ck+="; Secure";
    res.headers["Set-Cookie"]=ck;
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
  auto check_auth=[cfg](const std::string& cookie_hdr)->bool{
    if(cookie_hdr.empty()) return false;
    if(cfg.secret_prev.empty()) return verify_session_cookie(cfg.secret_key, cookie_hdr).has_value();
    return verify_session_cookie_dual(cfg.secret_key, cfg.secret_prev, cookie_hdr).has_value();
  };
  r.add("GET","/admin/dashboard", [cfg,check_auth](const Request& req){
    auto it=req.headers.find("Cookie");
    if(it==req.headers.end() || !check_auth(it->second)){
      Response rr; rr.status=302; rr.headers["Location"]="/login?next=/admin/dashboard"; return rr;
    }
    return handlers::admin::dashboard_page(req);
  });
  r.add("GET","/admin/settings", [cfg,check_auth](const Request& req){
    auto it=req.headers.find("Cookie");
    if(it==req.headers.end() || !check_auth(it->second)){
      Response rr; rr.status=302; rr.headers["Location"]="/login?next=/admin/settings"; return rr;
    }
    return handlers::admin::settings_page(req);
  });
  r.add("GET","/admin/pengawas", [cfg,check_auth](const Request& req){
    auto it=req.headers.find("Cookie");
    if(it==req.headers.end() || !check_auth(it->second)){
      Response rr; rr.status=302; rr.headers["Location"]="/login?next=/admin/pengawas"; return rr;
    }
    return handlers::admin::pengawas_page(req);
  });
  r.add("GET","/admin/pengawas/:exam_id", [cfg,check_auth](const Request& req){
    auto it=req.headers.find("Cookie");
    if(it==req.headers.end() || !check_auth(it->second)){
      Response rr; rr.status=302; rr.headers["Location"]="/login?next=/admin/pengawas"; return rr;
    }
    return handlers::admin::pengawas_detail_page(req);
  });
  r.add("GET","/admin/submissions", [cfg,check_auth](const Request& req){
    auto it=req.headers.find("Cookie");
    if(it==req.headers.end() || !check_auth(it->second)){
      Response rr; rr.status=302; rr.headers["Location"]="/login?next=/admin/submissions"; return rr;
    }
    return handlers::admin::submissions_page(req);
  });
  r.add("GET","/admin/api/stats", admin_api(handlers::admin::dashboard_stats));
  r.add("GET","/admin/api/saas-settings", admin_api(handlers::admin::settings_page));
  r.add("POST","/admin/api/saas-settings", admin_api(handlers::admin::update_settings));
  r.add("GET","/admin/api/users", admin_api(handlers::admin::list_users));
  r.add("GET","/admin/api/users/:id", admin_api(handlers::admin::list_users));
  r.add("POST","/admin/api/users", admin_api(handlers::admin::create_user));
  r.add("PUT","/admin/api/users/:id", admin_api(handlers::admin::edit_user));
  r.add("DELETE","/admin/api/users/:id", admin_api(handlers::admin::delete_user));
  r.add("POST","/admin/api/instansi/update", admin_api(handlers::admin::instansi_update));
  r.add("POST","/admin/api/change-password", admin_api([](const Request&){ Response rr; rr.json(200,"{\"ok\":true}"); return rr; }));
  r.add("GET","/admin/api/vouchers", admin_api(handlers::admin::list_vouchers));
  r.add("GET","/admin/api/vouchers/mine", admin_api(handlers::admin::list_vouchers));
  r.add("POST","/admin/api/vouchers/redeem", admin_api(handlers::admin::redeem_voucher));
  r.add("POST","/admin/api/vouchers/activate", admin_api(handlers::admin::activate_voucher));
  r.add("GET","/admin/api/vouchers/audit-logs", admin_api(handlers::admin::list_vouchers));
  r.add("GET","/admin/api/packages", admin_api(handlers::admin::list_vouchers));
  r.add("GET","/admin/api/exams", admin_api(handlers::admin::list_admin_exams));
  r.add("POST","/admin/api/exams", admin_api(handlers::admin::create_exam));
  r.add("POST","/admin/api/upload", admin_api(handlers::admin::create_exam));
  r.add("PUT","/admin/api/exams/:id", admin_api(handlers::admin::update_exam));
  r.add("DELETE","/admin/api/exams/:id", admin_api(handlers::admin::delete_exam));
  r.add("POST","/admin/api/exams/:exam_id/toggle", admin_api(handlers::admin::update_exam));
  r.add("POST","/admin/api/exams/:exam_id/delete", admin_api(handlers::admin::delete_exam));
  r.add("POST","/admin/api/exams/:exam_id/edit", admin_api(handlers::admin::update_exam));
  r.add("POST","/admin/api/exams/:exam_id/start", admin_api(handlers::admin::update_exam));
  r.add("POST","/admin/api/exams/:exam_id/stop", admin_api(handlers::admin::update_exam));
  r.add("POST","/admin/api/exams/:exam_id/regenerate-token", admin_api(handlers::admin::update_exam));
  r.add("GET","/admin/api/exams/:id/export", admin_api(handlers::admin::export_xlsx));
  r.add("GET","/admin/api/submissions", admin_api(handlers::admin::list_submissions));
  r.add("GET","/admin/api/submissions/:id/detail", admin_api(handlers::admin::submission_detail));
  r.add("GET","/admin/api/submissions/export", admin_api(handlers::admin::export_xlsx));
  r.add("GET","/admin/api/queue/status", admin_api(handlers::admin::queue_status));
  r.add("POST","/admin/api/submissions/:id/delete", admin_api(handlers::admin::delete_submission));
  r.add("GET","/admin/api/pengawas/exams", admin_api(handlers::admin::pengawas_exams));
  r.add("GET","/admin/api/pengawas/exams/:exam_id/submissions", admin_api(handlers::admin::pengawas_submissions));
  r.add("GET","/admin/api/pengawas/exams/:exam_id/approvals", admin_api(handlers::admin::pending_approvals));
  r.add("POST","/admin/api/pengawas/exams/:exam_id/approvals/:mac_address", admin_api(handlers::admin::set_approval));
  r.add("GET","/admin/api/pengawas/exams/:exam_id/auto-approve", admin_api(handlers::admin::get_auto_approve));
  r.add("POST","/admin/api/pengawas/exams/:exam_id/auto-approve", admin_api(handlers::admin::set_auto_approve));
  r.add("GET","/admin/api/system-apps", admin_api(handlers::admin::settings_page));
  r.add("POST","/admin/api/system-apps", admin_api(handlers::admin::update_settings));
}

} // namespace examvan
