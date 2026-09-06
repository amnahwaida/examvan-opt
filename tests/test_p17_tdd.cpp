#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include "handlers/api/exams.hpp"
#include "handlers/admin/exams.hpp"
#include "handlers/auth/login.hpp"
#include "handlers/auth/logout.hpp"
#include "http/router.hpp"
#include "http/router_full.hpp"
#include "config/config.hpp"
#include "store/exam_store.hpp"

using namespace examvan;

static std::string p17_read(const std::string& p) {
  std::ifstream f(p);
  if (!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)), {});
}

static std::string p17_line_at(const std::string& src, size_t pos) {
  size_t b = src.rfind('\n', pos == 0 ? 0 : pos - 1);
  b = (b == std::string::npos) ? 0 : b + 1;
  size_t e = src.find('\n', pos);
  if (e == std::string::npos) e = src.size();
  return src.substr(b, e - b);
}

static bool p17_route_gated(const std::string& src, const std::string& route, const std::string& gate) {
  auto p = src.find(route);
  if (p == std::string::npos) return false;
  return p17_line_at(src, p).find(gate) != std::string::npos;
}

TEST(P17_C1, VoucherRedeemActivateMineNotSuperadminOnly) {
  auto c = p17_read("src/http/router_full.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_FALSE(p17_route_gated(c, "vouchers/mine", "superadmin")) << "mine must be reachable by non-superadmin";
  EXPECT_FALSE(p17_route_gated(c, "vouchers/redeem", "superadmin")) << "redeem must be reachable by non-superadmin";
  EXPECT_FALSE(p17_route_gated(c, "vouchers/activate", "superadmin")) << "activate must be reachable by non-superadmin";
}

TEST(P17_C2, SelfServicePasswordAndInstansiNotSuperadminOnly) {
  auto c = p17_read("src/http/router_full.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_FALSE(p17_route_gated(c, "change-password", "superadmin"));
  EXPECT_FALSE(p17_route_gated(c, "instansi/update", "superadmin"));
}

TEST(P17_C3, SystemAppsBypassSmallBodyLimit) {
  auto c = p17_read("src/http/router_full.cpp");
  ASSERT_FALSE(c.empty());
  auto p = c.find("5*1024*1024");
  ASSERT_NE(p, std::string::npos);
  EXPECT_NE(p17_line_at(c, p).find("system-apps"), std::string::npos)
      << "5MB guard must exempt /system-apps: " << p17_line_at(c, p);
}

TEST(P17_L1, FrontendAccepts201OnSystemAppUpload) {
  auto c = p17_read("static/js/settings-system-apps.js");
  ASSERT_FALSE(c.empty());
  auto p = c.find("xhr.status === 200");
  ASSERT_NE(p, std::string::npos);
  EXPECT_NE(c.substr(p, 60).find("201"), std::string::npos) << "upload handler must accept 200||201";
}

TEST(P17_C4, WorkerRevokesApprovalAfterCommit) {
  auto c = p17_read("src/queue/submission_queue.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("DELETE FROM exam_approvals"), std::string::npos)
      << "worker must DELETE approval after persist";
}

TEST(P17_C5, UniqueIndexAndOnConflictUpsert) {
  auto m = p17_read("src/store/exam_store_postgres.cpp");
  ASSERT_FALSE(m.empty());
  EXPECT_NE(m.find("submissions_exam_mac"), std::string::npos) << "migrate must create UNIQUE submissions index";
  auto q = p17_read("src/queue/submission_queue.cpp");
  ASSERT_FALSE(q.empty());
  EXPECT_NE(q.find("ON CONFLICT"), std::string::npos) << "worker upsert must use ON CONFLICT";
}

TEST(P17_C7, SubmitValidatesRequiredIdentityFields) {
  auto c = p17_read("src/handlers/api/exams.cpp");
  ASSERT_FALSE(c.empty());
  auto s = c.find("Response submit_exam");
  auto e = c.find("Response exam_result");
  ASSERT_NE(s, std::string::npos);
  ASSERT_NE(e, std::string::npos);
  EXPECT_NE(c.substr(s, e - s).find("identity_fields"), std::string::npos)
      << "submit_exam must validate required identity_fields";
}

static void p17_make_started_exam(int& out_id, std::string& out_token, const std::string& idf) {
  examvan::handlers::admin::clear_exams_for_testing();
  setenv("R2_ACCESS_KEY_ID", "test", 1);
  setenv("R2_SECRET_ACCESS_KEY", "test", 1);
  setenv("R2_ENDPOINT", "https://test.r2.cloudflarestorage.com", 1);
  setenv("R2_BUCKET", "test", 1);
  Request cr;
  cr.body = "name=P17Identity&file_path=/tmp/a.pdf&size_bytes=100";
  auto created = examvan::handlers::admin::create_exam(cr);
  ASSERT_EQ(created.status, 201) << created.body;
  auto p = created.body.find("\"id\":");
  ASSERT_NE(p, std::string::npos);
  auto q = created.body.find_first_of(",}", p);
  out_id = std::stoi(created.body.substr(p + 5, q - p - 5));
  examvan::store::active_store()->update(out_id, [&](examvan::models::Exam& ex) {
    ex.status = "active";
    ex.exam_started_at = "2026-08-31T00:00:00Z";
    if (!idf.empty()) ex.identity_fields = idf;
  });
  auto exam = examvan::store::active_store()->get_by_id(out_id);
  ASSERT_TRUE(exam.has_value());
  out_token = exam->token;
}

TEST(P17_C7, SubmitRejectsMissingRequiredIdentity) {
  int eid = 0;
  std::string tok;
  p17_make_started_exam(eid, tok, "[{\"key\":\"nisn\",\"label\":\"NISN\",\"required\":true}]");
  handlers::api::set_submit_enqueue_hook_for_test([](const queue::SubmissionJob&) {});
  Request req;
  req.params["exam_id"] = std::to_string(eid);
  req.headers["X-Exam-Token"] = tok;
  req.body = "{\"mac_address\":\"AA:BB:CC:DD:EE:FF\",\"answers\":{\"1\":\"A\"},\"identity_data\":{}}";
  auto res = handlers::api::submit_exam(req);
  handlers::api::set_submit_enqueue_hook_for_test(nullptr);
  examvan::store::active_store()->clear_all();
  EXPECT_EQ(res.status, 400) << res.body;
}

TEST(P17_C7, SubmitAcceptsCompleteRequiredIdentity) {
  int eid = 0;
  std::string tok;
  p17_make_started_exam(eid, tok, "[{\"key\":\"nisn\",\"label\":\"NISN\",\"required\":true}]");
  bool enqueued = false;
  handlers::api::set_submit_enqueue_hook_for_test([&](const queue::SubmissionJob&) { enqueued = true; });
  Request req;
  req.params["exam_id"] = std::to_string(eid);
  req.headers["X-Exam-Token"] = tok;
  req.body = "{\"mac_address\":\"AA:BB:CC:DD:EE:FF\",\"answers\":{\"1\":\"A\"},\"identity_data\":{\"nisn\":\"123456\"}}";
  auto res = handlers::api::submit_exam(req);
  handlers::api::set_submit_enqueue_hook_for_test(nullptr);
  examvan::store::active_store()->clear_all();
  EXPECT_EQ(res.status, 202) << res.body;
  EXPECT_TRUE(enqueued);
}

TEST(P17_M3, StudentRoutesHonorProtobufMandatory) {
  auto c = p17_read("src/http/router_full.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("require_protobuf"), std::string::npos)
      << "student POST routes must be wrapped with require_protobuf";
}

TEST(P17_M3, StudentSubmitReturns415ForJsonWhenMandatory) {
  Config cfg;
  cfg.protobuf_mandatory = true;
  Router r;
  register_full_routes(r, cfg);
  Request req;
  req.method = "POST";
  req.path = "/api/exams/1/submit";
  req.body = "{\"mac_address\":\"AA\"}";
  req.headers["Content-Type"] = "application/json";
  auto res = r.dispatch(req);
  EXPECT_EQ(res.status, 415) << res.body;
}

TEST(P17_C6, SubmitAvoidsNestedActiveStore) {
  auto c = p17_read("src/handlers/api/exams.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("exam_snapshot"), std::string::npos)
      << "submit path must snapshot exam instead of nested active_store() calls";
}

TEST(P17_M2, TokenRotationUsesSelectForUpdate) {
  auto c = p17_read("src/handlers/api/exams.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("FOR UPDATE"), std::string::npos)
      << "token rotation must SELECT ... FOR UPDATE before memory sync";
}

TEST(P17_M1, PlainCsrfFallbackDevOnly) {
  auto login = p17_read("src/handlers/auth/login.cpp");
  auto router = p17_read("src/http/router_full.cpp");
  auto logout = p17_read("src/handlers/auth/logout.cpp");
  ASSERT_FALSE(login.empty());
  ASSERT_FALSE(router.empty());
  ASSERT_FALSE(logout.empty());
  auto gated = [](const std::string& src, const std::string& needle) {
    auto p = src.find(needle);
    if (p == std::string::npos) return true;
    size_t b = (p > 160) ? p - 160 : 0;
    return src.substr(b, p - b + needle.size()).find("is_development") != std::string::npos;
  };
  EXPECT_TRUE(gated(login, "extract_cookie(cookie_hdr,\"csrf_token\")")) << "login fallback must be dev-only";
  EXPECT_TRUE(gated(router, "extract_cookie(cookie_hdr,\"csrf_token\")")) << "router fallback must be dev-only";
  EXPECT_TRUE(gated(logout, "extract_cookie(ck,\"csrf_token\")")) << "logout fallback must be dev-only";
}

TEST(P17_L6, HostMismatchRejected) {
  handlers::auth::clear_users_for_test();
  handlers::auth::set_user_for_test("guru", "pass123", "guru");
  Config cfg;
  cfg.secret_key = std::string(32, 'x');
  Request req;
  req.body = "username=guru&password=pass123";
  req.headers["Cookie"] = "__Host-csrf_token=correct-token";
  req.headers["X-CSRF-Token"] = "wrong-token";
  req.headers["Accept"] = "application/json";
  auto res = handlers::auth::login_handler(req, cfg);
  handlers::auth::clear_users_for_test();
  EXPECT_EQ(res.status, 403);
}

TEST(P17_L6, PlainFallbackRejectedInProduction) {
  handlers::auth::clear_users_for_test();
  handlers::auth::set_user_for_test("guru", "pass123", "guru");
  Config cfg;
  cfg.secret_key = std::string(32, 'x');
  Request req;
  req.body = "username=guru&password=pass123";
  req.headers["Cookie"] = "csrf_token=test-csrf-token";
  req.headers["X-CSRF-Token"] = "test-csrf-token";
  req.headers["Accept"] = "application/json";
  auto res = handlers::auth::login_handler(req, cfg);
  handlers::auth::clear_users_for_test();
  EXPECT_EQ(res.status, 403) << "plain fallback must not pass in production: " << res.body;
}

TEST(P17_M4, LogoutFieldNameInSource) {
  auto c = p17_read("src/handlers/auth/logout.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("_csrf_token"), std::string::npos) << "logout must accept _csrf_token field";
}

TEST(P17_M4, LogoutAcceptsUnderscoreCsrfTokenField) {
  Request req;
  req.body = "_csrf_token=test-csrf-token";
  req.headers["Cookie"] = "__Host-csrf_token=test-csrf-token";
  auto res = handlers::auth::logout_handler(req);
  EXPECT_EQ(res.status, 200) << res.body;
}

TEST(P17_M5, OperatorPengawasScopedToSameInstansi) {
  auto c = p17_read("src/handlers/admin/pengawas.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("instansi"), std::string::npos)
      << "operator pengawas_exams must filter same-instansi";
}

TEST(P17_M6, JobResultCarriesIdentityFingerprint) {
  auto c = p17_read("src/queue/submission_queue.hpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("identity_hash"), std::string::npos) << "JobResult must carry identity fingerprint";
  auto e = p17_read("src/handlers/api/exams.cpp");
  ASSERT_FALSE(e.empty());
  EXPECT_NE(e.find("identity_hash"), std::string::npos) << "/result must validate identity fingerprint";
}

TEST(P17_M7, AuditLogsUseLogsKey) {
  auto c = p17_read("src/handlers/admin/pengawas.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("\"logs\""), std::string::npos) << "audit-log response must use logs key";
}

TEST(P17_L2, RetryHasBackoff) {
  auto c = p17_read("src/queue/submission_queue.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_NE(c.find("backoff"), std::string::npos) << "worker retry must back off";
}

TEST(P17_L3, RequeueChecksLpushResult) {
  auto c = p17_read("src/queue/submission_queue.cpp");
  ASSERT_FALSE(c.empty());
  auto p = c.find("bool SubmissionQueue::requeue");
  ASSERT_NE(p, std::string::npos);
  EXPECT_NE(c.substr(p, 700).find("lpush_checked_"), std::string::npos)
      << "requeue must consult checked LPUSH result";
}

TEST(P17_L4, StopJoinsBatchBeforeWorkers) {
  auto c = p17_read("src/queue/submission_queue.cpp");
  ASSERT_FALSE(c.empty());
  auto p = c.find("void Worker::stop()");
  ASSERT_NE(p, std::string::npos);
  std::string win = c.substr(p, 500);
  auto pb = win.find("batch_th_");
  auto pw = win.find("workers_");
  ASSERT_NE(pb, std::string::npos);
  ASSERT_NE(pw, std::string::npos);
  EXPECT_LT(pb, pw) << "stop() must join batch thread before workers";
}

TEST(P17_L5, MaskTokenHidesShortSecretLength) {
  auto c = p17_read("src/handlers/admin/settings.cpp");
  ASSERT_FALSE(c.empty());
  EXPECT_EQ(c.find("std::string(t.size(),'*')"), std::string::npos)
      << "mask_token must use fixed mask for short secrets";
}

TEST(P17_L2, BackoffGrowsWithRetries) {
  EXPECT_LT(examvan::queue::retry_backoff_ms(0), examvan::queue::retry_backoff_ms(2));
  EXPECT_LE(examvan::queue::retry_backoff_ms(10), 5000);
  EXPECT_GT(examvan::queue::retry_backoff_ms(1), 0);
}

TEST(P17_L3, RequeueReturnsFalseWhenLpushFails) {
  using namespace examvan::queue;
  SubmissionQueue q([](const std::string&, const std::string&) {}, nullptr, nullptr);
  q.set_lpush_checked([](const std::string&, const std::string&) { return false; });
  SubmissionJob job;
  job.job_id = "abc";
  EXPECT_FALSE(q.requeue(job));
  q.set_lpush_checked([](const std::string&, const std::string&) { return true; });
  EXPECT_TRUE(q.requeue(job));
}

TEST(P17_M6, JobResultJsonCarriesIdentityHash) {
  examvan::queue::JobResult r;
  r.job_id = "j1";
  r.exam_id = 7;
  r.mac_address = "AA";
  r.identity_data = "{\"nisn\":\"1\"}";
  r.identity_hash = examvan::queue::identity_fingerprint({{"nisn", "1"}});
  r.success = true;
  EXPECT_NE(r.to_json().find("identity_hash"), std::string::npos);
  EXPECT_NE(r.to_json().find(r.identity_hash), std::string::npos);
  EXPECT_EQ(examvan::queue::identity_fingerprint({{"nisn", "1"}}),
            examvan::queue::identity_fingerprint({{"nisn", "1"}}));
  EXPECT_NE(examvan::queue::identity_fingerprint({{"nisn", "1"}}),
            examvan::queue::identity_fingerprint({{"nisn", "2"}}));
}

TEST(P17_M6, ResultRejectsMismatchedIdentity) {
  int eid = 0;
  std::string tok;
  p17_make_started_exam(eid, tok, "");
  auto exam = examvan::store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  std::map<std::string, std::string> idmap{{"nisn", "111"}};
  std::string fp = examvan::queue::identity_fingerprint(idmap);
  handlers::api::set_result_lookup_hook_for_test(
      [fp](const std::string&) { return "{\"job_id\":\"jj\",\"exam_id\":-1,\"mac_address\":\"AA\",\"identity_hash\":\"" + fp + "\",\"success\":true,\"score\":90}"; });
  handlers::api::set_device_approved_hook_for_test([](int, const std::string&) { return true; });
  Request req;
  req.params["exam_id"] = std::to_string(eid);
  req.query = "job_id=jj&mac_address=AA&identity_data={\"nisn\":\"999\"}";
  auto res = handlers::api::exam_result(req);
  handlers::api::set_result_lookup_hook_for_test(nullptr);
  handlers::api::set_device_approved_hook_for_test(nullptr);
  examvan::store::active_store()->clear_all();
  EXPECT_EQ(res.status, 403) << res.body;
}
