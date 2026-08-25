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
