#include <gtest/gtest.h>
#include "http/router_full.hpp"
#include "config/config.hpp"
#include "handlers/public/hasil.hpp"
#include <fstream>
#include <string>

using namespace examvan;

static std::string slurp(const std::string& p){
  std::ifstream f(p); if(!f) return ""; return std::string((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
}

TEST(Characterization, HealthGolden) {
  Config cfg; cfg.version="2.7.2";
  Router r; register_full_routes(r,cfg);
  Request req; req.method="GET"; req.path="/api/health";
  auto res=r.dispatch(req);
  EXPECT_EQ(res.status,200);
  /* Golden Go health.go: status "healthy" + 6 key json-schema */
  EXPECT_NE(res.body.find("\"status\":\"healthy\""), std::string::npos);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
  EXPECT_NE(res.body.find("required_app_version"), std::string::npos);
  EXPECT_NE(res.body.find("server_time_utc"), std::string::npos);
}

TEST(Characterization, PublicHasilStructure) {
  Config cfg; Router r; register_full_routes(r,cfg);
  Request req; req.method="GET"; req.path="/hasil";
  auto res=r.dispatch(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("Cek Hasil"), std::string::npos);
  examvan::models::Exam e; e.token="ABC123"; e.name="Test"; e.public_results=1;
  examvan::handlers::public_::set_exam_for_test("ABC123", e);
  req.path="/hasil/ABC123"; req.params["token"]="ABC123";
  auto res2=r.dispatch(req);
  EXPECT_EQ(res2.status,200);
  examvan::handlers::public_::clear_exams_for_test();
}

TEST(Characterization, ShortUrlRedirect) {
  Config cfg; Router r; register_full_routes(r,cfg);
  Request req; req.method="GET"; req.path="/MYTOKEN"; req.params["token"]="MYTOKEN";
  auto res=r.dispatch(req);
  EXPECT_EQ(res.status,302);
  EXPECT_EQ(res.headers["Location"], "/hasil/MYTOKEN");
}

TEST(Characterization, AdminRedirectNotShadowedByTokenCatchall) {
  /* /admin (1 segmen) harus redirect ke dashboard, TIDAK tertangkap
   * catch-all /:token → /hasil/admin. Router first-match. */
  Config cfg; cfg.secret_key=std::string(32,'x');
  Router r; register_full_routes(r,cfg);
  Request req; req.method="GET"; req.path="/admin";
  auto res=r.dispatch(req);
  EXPECT_EQ(res.status,302);
  EXPECT_EQ(res.headers["Location"], "/admin/dashboard");
}

TEST(Characterization, LoginRateLimitedPerIp) {
  /* POST /login di-rate-limit 10/mnt per-IP di lapisan C++ (bukan hanya
   * nginx). IP unik agar tidak mengganggu test lain (limiter statis). */
  Config cfg; cfg.secret_key=std::string(32,'x');
  Router r; register_full_routes(r,cfg);
  std::string ip="203.0.113.77";
  int ok_401=0, got_429=0;
  for(int i=0;i<12;i++){
    Request req; req.method="POST"; req.path="/login";
    req.headers["X-Real-IP"]=ip;
    req.headers["Cookie"]="csrf_token=test-token";
    req.headers["X-CSRF-Token"]="test-token";
    req.headers["Accept"]="application/json";
    req.body="username=nobody&password=wrong&_csrf=test-token";
    auto res=r.dispatch(req);
    if(res.status==429) got_429++;
    else if(res.status==401) ok_401++;
  }
  EXPECT_EQ(ok_401,10) << "first 10 invalid logins should be 401 (got "<<ok_401<<")";
  EXPECT_EQ(got_429,2) << "11th-12th login from same IP should be rate-limited (got "<<got_429<<" 429s)";
}

TEST(Characterization, ApiVersionWebClientAllowed) {
  /* Semantik Go: TANPA header X-App-Version → izinkan (client web);
   * required kosong (fresh DB tanpa system_apps) → izinkan semua. */
  Config cfg; Router r; register_full_routes(r,cfg);
  Request req; req.method="GET"; req.path="/api/exams";
  auto res=r.dispatch(req);
  EXPECT_EQ(res.status,200);
}

TEST(Characterization, TemplatesExist) {
  EXPECT_FALSE(slurp("templates/public/hasil.html").empty());
  EXPECT_FALSE(slurp("static/css/theme.css").empty());
}
