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
#include "handlers/auth/register.hpp"
#include "handlers/auth/recovery.hpp"
#include "handlers/auth/auth_helpers.hpp"
#include "handlers/public/template_helper.hpp"
#include "session/cookie.hpp"
#include "session/csrf.hpp"
#include "middleware/ratelimit.hpp"
#include "middleware/body_limit.hpp"
#include "middleware/cors.hpp"
#include "store/exam_store.hpp"
#ifdef HAS_LIBPQ
#include "db/pool_real.hpp"
#include "db/pool.hpp"
#endif
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
  /* role_req: "superadmin" → route hanya untuk superadmin (manajemen users/
   * vouchers/settings/packages). Kosong → route data/eksam untuk semua
   * user active (guru/pengawas). require_role() di middleware/auth.hpp. */
  auto admin_api=[cfg](Handler h, std::string role_req={}, std::string scope={})->Handler{
    return [cfg,h,role_req,scope](const Request& req)->Response{
      if(req.body.size()>5*1024*1024){ Response rr; rr.status=413; rr.body="payload too large"; return rr; }
      std::string ip="global";
      auto it_ip=req.headers.find("X-Real-IP");
      if(it_ip!=req.headers.end()) ip=it_ip->second;
      if(!g_admin_rl.allow(ip)){ Response rr; rr.status=429; rr.json(429,"{\"error\":\"rate limit exceeded\"}"); return rr; }
      /* Cek ukuran body TANPA mengeksekusi handler: body_limit(next)
       * memanggil next saat body lolos, dan handler dipanggil sekali lagi
       * di bawah (dengan header internal) — memakai body_limit di sini
       * membuat SETIAP mutasi admin dieksekusi DUA KALI (INSERT ganda,
       * created_by=0 di run pertama yang dibuang, dst). */
      std::string key=cfg.secret_key;
      std::string prev=cfg.secret_prev;
      auto it=req.headers.find("Cookie");
      bool ok=false;
      SessionData sess;
      if(it!=req.headers.end()){
        if(prev.empty()){ auto s=verify_session_cookie(key,it->second); ok=s.has_value(); if(ok) sess=*s; }
        else { auto s=verify_session_cookie_dual(key,prev,it->second); ok=s.has_value(); if(ok) sess=*s; }
        if(!ok) ok=middleware::is_authenticated(req,key,&sess);
      }
      if(!ok){
        Response rr; rr.status=401; rr.json(401,"{\"success\":false,\"message\":\"unauthorized\"}"); return rr;
      }
      // Revalidasi session terhadap PG (paritas Go auth.go): user yang sudah
      // di-suspend/dihapus tidak boleh lanjut pakai session lama. Fail-open
      // saat PG tidak dikonfigurasi/tak terjangkau (dev in-memory, unit test).
#ifdef HAS_LIBPQ
      {
        bool pg_up=false, pg_ok=true;
        try{
          std::string db_url=Config::load().database_url;
          if(db_url.empty()) if(auto* e=getenv("DATABASE_URL")) db_url=e;
          if(!db_url.empty()){
            std::string ci=pg_conninfo_from_url(db_url);
            if(ci.empty()) ci=db_url;
            examvan::db::RealPool real(ci, 1);
            if(real.connect()){
              if(auto c=real.acquire()){
                pg_up=true;
                auto res=real.exec_params(c.get(),"SELECT status FROM admin_users WHERE id=$1",{std::to_string(sess.admin_id)});
                if(!res || PQresultStatus(res.get())!=PGRES_TUPLES_OK || PQntuples(res.get())==0){
                  pg_ok=false; // user sudah dihapus
                } else {
                  std::string st=PQgetvalue(res.get(),0,0);
                  if(st!="active") pg_ok=false; // suspended / pending_otp
                }
              }
            }
          }
        }catch(...){}
        if(pg_up && !pg_ok){
          Response rr; rr.status=401; rr.json(401,"{\"success\":false,\"message\":\"unauthorized\"}"); return rr;
        }
      }
#endif
      // Role gate: manajemen (users/vouchers/settings/packages) hanya
      // superadmin. Sebelumnya TIDAK ada cek role — guru/pengawas yang
      // aktif bisa hapus user, ubah saas_settings, dsb (privilege escalation).
      if(!role_req.empty()){
        bool ok_role = sess.is_super_admin || sess.role.find(role_req)!=std::string::npos;
        if(!ok_role){
          Response rr; rr.status=403; rr.json(403,"{\"success\":false,\"message\":\"forbidden\"}"); return rr;
        }
      }
      // C5: CSRF — mutasi (POST/PUT/DELETE/PATCH) admin wajib header
      // X-CSRF-Token/X-XSRF-Token yang cocok dengan cookie csrf_token
      // (double-submit, paritas Go CSRFRequired; frontend admin-core.js
      // mengirim token dari <meta name=csrf-token>). GET/HEAD/OPTIONS bebas.
      {
        std::string m=req.method;
        bool mutating = m=="POST" || m=="PUT" || m=="DELETE" || m=="PATCH";
        if(mutating){
          std::string cookie_hdr;
          auto itc=req.headers.find("Cookie");
          if(itc!=req.headers.end()) cookie_hdr=itc->second;
          std::string session_csrf=extract_cookie(cookie_hdr,"csrf_token");
          std::string tok;
          for(auto& kv: req.headers){
            std::string k=kv.first; for(char& ch:k) ch=tolower((unsigned char)ch);
            if(k=="x-csrf-token" || k=="x-xsrf-token"){ tok=kv.second; break; }
          }
          if(session_csrf.empty() || tok.empty() || !verify_csrf(session_csrf, tok)){
            Response rr; rr.status=403; rr.json(403,"{\"success\":false,\"message\":\"CSRF token tidak valid. Silakan refresh halaman.\"}"); return rr;
          }
        }
      }
      // C7: ownership scope "exam" — route per-exam (detail/toggle/delete/
      // questions/delegate/export) hanya untuk superadmin, PEMILIK
      // (created_by), atau user yang didelegasi (delegated_to). Tanpa ini guru
      // instansi mana pun bisa baca/ubah/hapus exam lintas sekolah (termasuk
      // kunci jawaban). Scope memakai store (id di path) — paritas Go
      // checkExamOwnership (tanpa dimensi instansi: store C++ tidak punya
      // kolom instansi; pemilik/delegasi/superadmin tercakup).
      if(scope=="exam"){
        // Ekstrak exam id dari path (:id atau :exam_id).
        std::string eid;
        auto pid=req.params.find("exam_id");
        if(pid!=req.params.end()) eid=pid->second;
        else { auto p2=req.params.find("id"); if(p2!=req.params.end()) eid=p2->second; }
        bool owner_ok=sess.is_super_admin;
        if(!owner_ok && !eid.empty()){
          try{
            int exam_id=std::stoi(eid);
            auto e=store::active_store()->get_by_id(exam_id);
            if(e){
              if(e->created_by==sess.admin_id) owner_ok=true;
              if(!owner_ok && e->delegated_to && *e->delegated_to==sess.admin_id) owner_ok=true;
            }
          }catch(...){}
        }
        // Tanpa id di path (mis. list/bulk) → biarkan handler memutuskan.
        if(!eid.empty() && !owner_ok){
          Response rr; rr.status=403; rr.json(403,"{\"success\":false,\"message\":\"forbidden\"}"); return rr;
        }
      }
      // C8: scope "submission" — resolve submission id → exam_id lalu terapkan
      // kepemilikan exam (superadmin | created_by | delegated). Tanpa PG
      // (unit test / dev memory) fail-open, konsisten dgn revalidasi sesi.
      if(scope=="submission"){
        std::string sid;
        auto p3=req.params.find("id");
        if(p3!=req.params.end()) sid=p3->second;
        bool sub_ok=sess.is_super_admin;
        if(!sub_ok && !sid.empty()){
#ifdef HAS_LIBPQ
          try{
            std::string db_url=Config::load().database_url;
            if(db_url.empty()) if(auto* e=getenv("DATABASE_URL")) db_url=e;
            if(!db_url.empty()){
              std::string ci=pg_conninfo_from_url(db_url);
              if(ci.empty()) ci=db_url;
              examvan::db::RealPool real(ci, 2);
              if(real.connect()){
                if(auto c=real.acquire()){
                  auto res=real.exec_params(c.get(),
                    "SELECT exam_id FROM submissions WHERE id=$1",{sid});
                  if(res && PQresultStatus(res.get())==PGRES_TUPLES_OK && PQntuples(res.get())>0){
                    int exam_id=0; try{ exam_id=std::stoi(PQgetvalue(res.get(),0,0)); }catch(...){}
                    auto e=store::active_store()->get_by_id(exam_id);
                    if(e){
                      if(e->created_by==sess.admin_id) sub_ok=true;
                      if(!sub_ok && e->delegated_to && *e->delegated_to==sess.admin_id) sub_ok=true;
                    }
                  }
                }
              }
            }
          }catch(...){}
#endif
        }
        if(!sid.empty() && !sub_ok){
          Response rr; rr.status=403; rr.json(403,"{\"success\":false,\"message\":\"forbidden\"}"); return rr;
        }
      }
      // Teruskan admin_id session ke handler via header internal (nilai dari
      // session terverifikasi, meng-overwrite apapun yang dikirim klien).
      // create_exam memakainya untuk created_by (FK exams_created_by_fkey).
      Request r2=req;
      r2.headers["X-Internal-Admin-Id"]=std::to_string(sess.admin_id);
      return h(r2);
    };
  };

  /* Mutasi public auth di-rate-limit per-IP di LAPISAN C++ (bukan hanya
   * nginx) — login 10/mnt, register/confirm/resend/forgot/reset 5/mnt,
   * /api/hasil 30/mnt (paritas Go; lihat doc alur-public). RateLimiter
   * statis per alur. client_ip() baca X-Real-IP/X-Forwarded-For yang di-set
   * nginx (meng-overwrite nilai klien) — aman di belakang proxy. */
  static middleware::RateLimiter g_auth_login_rl(10, std::chrono::minutes(1));
  static middleware::RateLimiter g_auth_register_rl(5, std::chrono::minutes(1));
  static middleware::RateLimiter g_auth_confirm_rl(5, std::chrono::minutes(1));
  static middleware::RateLimiter g_auth_resend_rl(5, std::chrono::minutes(1));
  static middleware::RateLimiter g_auth_forgot_rl(5, std::chrono::minutes(1));
  static middleware::RateLimiter g_auth_reset_rl(5, std::chrono::minutes(1));
  auto auth_ip=[](const Request& req)->std::string{ return handlers::auth::client_ip(req); };
  auto rl_wrap=[auth_ip](middleware::RateLimiter& lim, Handler h)->Handler{
    return [&lim, auth_ip, h](const Request& req)->Response{
      if(!lim.allow(auth_ip(req))){
        Response rr; rr.status=429;
        rr.json(429,"{\"error\":\"Terlalu banyak permintaan. Coba lagi nanti.\"}");
        return rr;
      }
      return h(req);
    };
  };

  r.add("GET","/login", [cfg](const Request& req){ return handlers::auth::login_page(req); });
  r.add("POST","/login", rl_wrap(g_auth_login_rl, [cfg](const Request& req){ return handlers::auth::login_handler(req, cfg); }));
  /* Alias /admin/login: template login & admin-core.js memakai path ini.
   * Tanpa alias, submit form login → fallback 404 JSON {"error":"not found"}. */
  r.add("GET","/admin/login", [cfg](const Request& req){ return handlers::auth::login_page(req); });
  r.add("POST","/admin/login", rl_wrap(g_auth_login_rl, [cfg](const Request& req){ return handlers::auth::login_handler(req, cfg); }));
  r.add("POST","/logout", [](const Request& req){ return handlers::auth::logout_handler(req); });
  r.add("GET","/logout", [](const Request& req){ return handlers::auth::logout_page(req); });
  r.add("GET","/register", handlers::auth::register_page);
  r.add("POST","/register", rl_wrap(g_auth_register_rl, [cfg](const Request& req){ return handlers::auth::register_handler(req, cfg); }));
  r.add("GET","/register/confirm", handlers::auth::register_confirm_page);
  r.add("POST","/register/confirm", rl_wrap(g_auth_confirm_rl, [cfg](const Request& req){ return handlers::auth::register_confirm_handler(req, cfg); }));
  r.add("POST","/register/resend", rl_wrap(g_auth_resend_rl, [cfg](const Request& req){ return handlers::auth::resend_otp(req, cfg); }));
  r.add("GET","/forgot-password", handlers::auth::forgot_password_page);
  r.add("POST","/forgot-password", rl_wrap(g_auth_forgot_rl, [cfg](const Request& req){ return handlers::auth::forgot_password_handler(req, cfg); }));
  r.add("GET","/reset-password", handlers::auth::reset_password_page);
  r.add("POST","/reset-password", rl_wrap(g_auth_reset_rl, [cfg](const Request& req){ return handlers::auth::reset_password_handler(req, cfg); }));
  r.add("GET","/download", handlers::public_::download_page);
  r.add("GET","/download/apk", handlers::public_::download_apk);
  r.add("GET","/download/app/:id", handlers::public_::download_system_app);
  r.add("GET","/hasil", handlers::public_::cek_hasil_page);
  r.add("GET","/hasil/:token", handlers::public_::hasil_page);
  /* /admin redirect harus SEBELUM catch-all /:token (router first-match):
   * tanpa ini, GET /admin (1 segmen) tertangkap /:token → 302 ke /hasil/admin
   * alih-alih /admin/dashboard. */
  r.add("GET","/admin", [](const Request&){ Response rr; rr.status=302; rr.headers["Location"]="/admin/dashboard"; return rr; });
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
  /* /api/hasil/:token di-rate-limit 30/mnt per-IP (paritas Go; lihat doc
   alur-public). Halaman /hasil memakai endpoint ini juga. */
  static middleware::RateLimiter g_hasil_api_rl(30, std::chrono::minutes(1));
  r.add("GET","/api/hasil/:token", rl_wrap(g_hasil_api_rl, handlers::public_::cek_hasil_api));
  r.add("POST","/api/webhook", handlers::api::webhook);

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
  r.add("GET","/admin/api/saas-settings", admin_api(handlers::admin::settings_page, "superadmin"));
  r.add("POST","/admin/api/saas-settings", admin_api(handlers::admin::update_settings, "superadmin"));
  r.add("GET","/admin/api/users", admin_api(handlers::admin::list_users, "superadmin"));
  r.add("GET","/admin/api/users/:id", admin_api(handlers::admin::user_detail, "superadmin"));
  r.add("POST","/admin/api/users", admin_api(handlers::admin::create_user, "superadmin"));
  r.add("PUT","/admin/api/users/:id", admin_api(handlers::admin::edit_user, "superadmin"));
  r.add("POST","/admin/api/users/:id/edit", admin_api(handlers::admin::edit_user, "superadmin"));
  r.add("DELETE","/admin/api/users/:id", admin_api(handlers::admin::delete_user, "superadmin"));
  r.add("POST","/admin/api/users/:id/delete", admin_api(handlers::admin::delete_user, "superadmin"));
  r.add("POST","/admin/api/users/:id/toggle-status", admin_api(handlers::admin::user_toggle_status, "superadmin"));
  r.add("POST","/admin/api/users/:id/verify", admin_api(handlers::admin::user_verify, "superadmin"));
  r.add("POST","/admin/api/users/:id/deactivate-package", admin_api(handlers::admin::user_deactivate_package, "superadmin"));
  r.add("POST","/admin/api/instansi/update", admin_api(handlers::admin::instansi_update, "superadmin"));
  r.add("POST","/admin/api/change-password", admin_api(handlers::admin::change_password, "superadmin"));
  r.add("GET","/admin/api/vouchers", admin_api(handlers::admin::list_vouchers, "superadmin"));
  r.add("POST","/admin/api/vouchers", admin_api(handlers::admin::create_voucher, "superadmin"));
  r.add("POST","/admin/api/vouchers/batch", admin_api(handlers::admin::create_vouchers_batch, "superadmin"));
  r.add("GET","/admin/api/vouchers/mine", admin_api(handlers::admin::vouchers_mine, "superadmin"));
  r.add("POST","/admin/api/vouchers/redeem", admin_api(handlers::admin::redeem_voucher, "superadmin"));
  r.add("POST","/admin/api/vouchers/activate", admin_api(handlers::admin::activate_voucher, "superadmin"));
  r.add("GET","/admin/api/vouchers/audit-logs", admin_api(handlers::admin::list_audit_logs, "superadmin"));
  r.add("POST","/admin/api/vouchers/:id/toggle", admin_api(handlers::admin::toggle_voucher, "superadmin"));
  r.add("POST","/admin/api/vouchers/:id/delete", admin_api(handlers::admin::delete_voucher, "superadmin"));
  r.add("GET","/admin/api/vouchers/:id/redemptions", admin_api(handlers::admin::voucher_redemptions, "superadmin"));
  r.add("GET","/admin/api/packages", admin_api(handlers::admin::list_packages, "superadmin"));
  r.add("POST","/admin/api/packages", admin_api(handlers::admin::save_packages, "superadmin"));
  r.add("GET","/admin/api/exams", admin_api(handlers::admin::list_admin_exams));
  r.add("POST","/admin/api/exams", admin_api(handlers::admin::create_exam));
  r.add("POST","/admin/api/upload", admin_api(handlers::admin::create_exam));
  r.add("PUT","/admin/api/exams/:id", admin_api(handlers::admin::update_exam,"","exam"));
  r.add("DELETE","/admin/api/exams/:id", admin_api(handlers::admin::delete_exam,"","exam"));
  r.add("POST","/admin/api/exams/bulk-toggle", admin_api(handlers::admin::bulk_toggle_exams));
  r.add("POST","/admin/api/exams/bulk-delete", admin_api(handlers::admin::bulk_delete_exams));
  r.add("GET","/admin/api/exams/:exam_id/delegate-data", admin_api(handlers::admin::delegate_data,"","exam"));
  r.add("POST","/admin/api/exams/:exam_id/delegate", admin_api(handlers::admin::delegate_exam,"","exam"));
  r.add("POST","/admin/api/exams/:exam_id/toggle", admin_api(handlers::admin::update_exam,"","exam"));
  r.add("POST","/admin/api/exams/:exam_id/delete", admin_api(handlers::admin::delete_exam,"","exam"));
  r.add("POST","/admin/api/exams/:exam_id/edit", admin_api(handlers::admin::update_exam,"","exam"));
  r.add("POST","/admin/api/exams/:exam_id/start", admin_api(handlers::admin::update_exam,"","exam"));
  r.add("POST","/admin/api/exams/:exam_id/stop", admin_api(handlers::admin::update_exam,"","exam"));
  r.add("POST","/admin/api/exams/:exam_id/regenerate-token", admin_api(handlers::admin::update_exam,"","exam"));
  r.add("POST","/admin/api/exams/:exam_id/edit-token", admin_api(handlers::admin::update_exam,"","exam"));
  r.add("POST","/admin/api/exams/:exam_id/token-mode", admin_api(handlers::admin::update_exam,"","exam"));
  r.add("GET","/admin/api/exams/:exam_id/questions", admin_api(handlers::admin::get_exam_questions,"","exam"));
  r.add("POST","/admin/api/exams/:exam_id/questions", admin_api(handlers::admin::save_exam_questions,"","exam"));
  r.add("GET","/admin/api/exams/:id/export", admin_api(handlers::admin::export_xlsx,"","exam"));
  r.add("GET","/admin/api/submissions", admin_api(handlers::admin::list_submissions));
  // Static export route must precede /:id/detail (router uses first-match).
  r.add("GET","/admin/api/submissions/export", admin_api(handlers::admin::export_xlsx));
  r.add("GET","/admin/api/submissions/:id/detail", admin_api(handlers::admin::submission_detail,"","submission"));
  r.add("GET","/admin/api/queue/status", admin_api(handlers::admin::queue_status));
  r.add("POST","/admin/api/submissions/:id/delete", admin_api(handlers::admin::delete_submission,"","submission"));
  r.add("GET","/admin/api/pengawas/exams", admin_api(handlers::admin::pengawas_exams));
  r.add("GET","/admin/api/pengawas/exams/:exam_id/submissions", admin_api(handlers::admin::pengawas_submissions,"","exam"));
  r.add("GET","/admin/api/pengawas/exams/:exam_id/approvals", admin_api(handlers::admin::pending_approvals,"","exam"));
  r.add("POST","/admin/api/pengawas/exams/:exam_id/approvals/:mac_address", admin_api(handlers::admin::set_approval,"","exam"));
  r.add("GET","/admin/api/pengawas/exams/:exam_id/auto-approve", admin_api(handlers::admin::get_auto_approve,"","exam"));
  r.add("POST","/admin/api/pengawas/exams/:exam_id/auto-approve", admin_api(handlers::admin::set_auto_approve,"","exam"));
  r.add("GET","/admin/api/system-apps", admin_api(handlers::admin::settings_page, "superadmin"));
  r.add("POST","/admin/api/system-apps", admin_api(handlers::admin::update_settings, "superadmin"));
}

} // namespace examvan
