#include <gtest/gtest.h>
#include "http/router_full.hpp"
#include "config/config.hpp"
#include "handlers/public/hasil.hpp"
#include "session/cookie.hpp"
#include "handlers/auth/login.hpp"
#include "store/exam_store.hpp"
#include <fstream>
#include <string>
#include <vector>

using namespace examvan;

// Bangun cookie session dengan role tertentu (payload = pola login.cpp).
static std::string session_cookie_for(const Config& cfg, int admin_id, const std::string& role){
  std::string payload=b64_encode("admin_id="+std::to_string(admin_id)+"&username=u&role=["+role+"]");
  return "examvan_session="+encode_cookie_value(cfg.secret_key, payload);
}

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

TEST(Characterization, HasilApiMultiSelectKeyNotLeakedOrCorrupted) {
  /* Soal multiple_choice punya "key":["A","B"] (array). strip_sensitive_keys
   * harus menghapus key utuh TANPA merusak JSON — regresi: dulu menyisakan
   * "B"], dan membocorkan sisa kunci jawaban ke pengunjung public. */
  Config cfg; cfg.secret_key=std::string(32,'x');
  Router r; register_full_routes(r,cfg);
  examvan::models::Exam e;
  e.token="MULTI1"; e.name="Tes Multi";
  e.questions_json=R"([{"number":1,"type":"multiple_choice","weight":4,"partial_scoring":true,"key":["A","B"],"choices":["A","B","C","D"]}])";
  e.public_results=1; e.show_answers=0; // non-logged → key must be stripped
  examvan::handlers::public_::set_exam_for_test("MULTI1", e);
  Request req; req.method="GET"; req.path="/api/hasil/MULTI1"; req.params["token"]="MULTI1";
  auto res=r.dispatch(req);
  examvan::handlers::public_::clear_exams_for_test();
  EXPECT_EQ(res.status,200);
  // Ambil hanya array "questions":[...] — identity_fields memakai "key"
  // sbg nama field yang sah, bukan kunci jawaban.
  auto qp=res.body.find("\"questions\":[");
  ASSERT_NE(qp, std::string::npos);
  size_t qstart=qp+std::string("\"questions\":[").size();
  // Cari kurung tutup array questions (luar string).
  size_t qend=qstart; bool in_str=false, esc=false; int depth=1;
  for(; qend<res.body.size(); ++qend){
    char c=res.body[qend];
    if(esc){ esc=false; continue; }
    if(c=='\\' && in_str){ esc=true; continue; }
    if(c=='"'){ in_str=!in_str; continue; }
    if(in_str) continue;
    if(c=='[') depth++;
    else if(c==']'){ if(--depth==0){ break; } }
  }
  std::string q=res.body.substr(qstart, qend-qstart);
  EXPECT_EQ(q.find("\"key\""), std::string::npos) << "answer key leaked in questions";
  EXPECT_EQ(q.find("\"answer\""), std::string::npos) << "answer field leaked in questions";
  // Tidak ada fragmen key yang tertinggal (dulu: "B"],) dan choices utuh.
  EXPECT_EQ(q.find("B\"]"), std::string::npos) << "key fragment left behind";
  EXPECT_NE(q.find("\"choices\":[\"A\",\"B\",\"C\",\"D\"]"), std::string::npos) << "choices should remain intact";
}

TEST(Characterization, CekHasilTokenQueryRedirects){
  /* Form "Cek Hasil" submit GET /hasil?token=AB12CD34 → harus redirect ke
   * /hasil/AB12CD34 (dulu token diabaikan, form seolah rusak). */
  Config cfg; Router r; register_full_routes(r,cfg);
  Request req; req.method="GET"; req.path="/hasil"; req.query="token=ab12cd34";
  auto res=r.dispatch(req);
  EXPECT_EQ(res.status,302);
  EXPECT_EQ(res.headers["Location"], "/hasil/AB12CD34"); // token di-uppercase
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

TEST(Characterization, AdminManagementRequiresSuperadmin) {
  /* admin_api dulu hanya cek session+status — guru/pengawas yang aktif bisa
   * panggil /admin/api/users (hapus user), vouchers, saas-settings dsb.
   * Sekarang route manajemen butuh superadmin → non-superadmin 403. */
  Config cfg; cfg.secret_key=std::string(32,'x');
  Router r; register_full_routes(r,cfg);
  // Non-superadmin (guru) → 403 pada route manajemen.
  Request req; req.method="GET"; req.path="/admin/api/users";
  req.headers["Cookie"]=session_cookie_for(cfg, 2, "guru");
  req.headers["X-CSRF-Token"]="x"; req.headers["Accept"]="application/json";
  auto res=r.dispatch(req);
  EXPECT_EQ(res.status,403) << "guru must be forbidden from user management";
  // Superadmin → bukan 403 (handler dipanggil; tanpa PG bisa 200/401, bukan 403).
  Request req2; req2.method="GET"; req2.path="/admin/api/users";
  req2.headers["Cookie"]=session_cookie_for(cfg, 1, "superadmin");
  req2.headers["Accept"]="application/json";
  auto res2=r.dispatch(req2);
  EXPECT_NE(res2.status,403) << "superadmin must not be forbidden";
  // Route data (exams) tetap terbuka utk guru.
  Request req3; req3.method="GET"; req3.path="/admin/api/exams";
  req3.headers["Cookie"]=session_cookie_for(cfg, 2, "guru");
  req3.headers["Accept"]="application/json";
  auto res3=r.dispatch(req3);
  EXPECT_NE(res3.status,403) << "guru can access exam data routes";
}

TEST(Characterization, ExamOwnershipScopedToCreatorOrSuperadmin) {
  /* C7: route per-exam (questions/delete) wajib scope kepemilikan — guru yang
   * bukan pembuat (created_by) exam tsb harus 403. Tanpa scope, guru instansi
   * mana pun bisa baca kunci jawaban / hapus exam lintas sekolah. */
  Config cfg; cfg.secret_key=std::string(32,'x');
  // Seed exam di store dengan created_by=1 (superadmin).
  store::active_store()->clear_all();
  examvan::models::Exam e; e.id=100; e.name="Milik Superadmin"; e.token="OWNABC12";
  e.status="active"; e.created_by=1; e.questions_json="[{\"number\":1,\"key\":\"A\"}]";
  store::active_store()->add(e);
  Router r; register_full_routes(r,cfg);
  // Guru lain (admin_id=2) minta questions exam 100 → 403.
  Request q; q.method="GET"; q.path="/admin/api/exams/100/questions";
  q.headers["Cookie"]=session_cookie_for(cfg, 2, "guru");
  q.headers["Accept"]="application/json";
  auto res=r.dispatch(q);
  EXPECT_EQ(res.status,403) << "non-owner guru must be forbidden from questions: " << res.body;
  // Superadmin (admin_id=1) → bukan 403.
  Request q2; q2.method="GET"; q2.path="/admin/api/exams/100/questions";
  q2.headers["Cookie"]=session_cookie_for(cfg, 1, "superadmin");
  q2.headers["Accept"]="application/json";
  auto res2=r.dispatch(q2);
  EXPECT_NE(res2.status,403) << "superadmin must not be forbidden: " << res2.body;
  store::active_store()->clear_all();
}

TEST(Characterization, AdminMutationRequiresCsrfCookieMatch) {
  /* C5: mutasi admin (POST/PUT/DELETE) wajib header X-CSRF-Token yang cocok
   * dgn cookie csrf_token. Tanpa token / token beda → 403; token cocok → lolos
   * gate CSRF (baru mungkin gagal di handler, bukan 403 CSRF). */
  Config cfg; cfg.secret_key=std::string(32,'x');
  Router r; register_full_routes(r,cfg);
  // Siapkan session superadmin + exam miliknya utk target POST.
  store::active_store()->clear_all();
  examvan::models::Exam e; e.id=200; e.name="CSRF Target"; e.token="CSRFAB12";
  e.status="inactive"; e.created_by=1;
  store::active_store()->add(e);
  std::string cookie=session_cookie_for(cfg, 1, "superadmin");
  // POST tanpa X-CSRF-Token → 403 CSRF.
  Request no_tok; no_tok.method="POST"; no_tok.path="/admin/api/exams/200/toggle";
  no_tok.headers["Cookie"]=cookie; no_tok.headers["Accept"]="application/json";
  auto r1=r.dispatch(no_tok);
  EXPECT_EQ(r1.status,403) << "mutation without CSRF token must be 403: " << r1.body;
  // POST dengan token tapi cookie csrf_token beda → 403.
  Request bad_tok=no_tok;
  bad_tok.headers["X-CSRF-Token"]="wrongtoken";
  auto r2=r.dispatch(bad_tok);
  EXPECT_EQ(r2.status,403) << "mutation with mismatched CSRF must be 403: " << r2.body;
  // POST dengan cookie csrf_token + header cocok → lolos gate (bukan 403 CSRF).
  // (Simulasikan cookie csrf_token yg juga di-set halaman: set cookie ganda.)
  Request ok=no_tok;
  std::string csrf="deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";
  ok.headers["Cookie"]=cookie+"; csrf_token="+csrf;
  ok.headers["X-CSRF-Token"]=csrf;
  auto r3=r.dispatch(ok);
  EXPECT_NE(r3.status,403) << "mutation with matching CSRF must pass the gate: " << r3.body;
  store::active_store()->clear_all();
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
