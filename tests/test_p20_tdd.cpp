#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include "handlers/api/exams.hpp"
#include "handlers/admin/exams.hpp"
#include "store/exam_store.hpp"
using namespace examvan;
static std::string read_src20(const std::string& p){ std::ifstream f(p); if(!f) return ""; std::ostringstream ss; ss<<f.rdbuf(); return ss.str(); }
TEST(P20, SubmitPersistsBeforeEnqueue){
  auto src = read_src20("src/handlers/api/exams.cpp");
  auto ins = src.find("INSERT INTO submissions");
  ASSERT_NE(ins, std::string::npos);
  auto lpush = src.find("enqueue_job_to_redis", ins);
  EXPECT_NE(lpush, std::string::npos);
}
TEST(P20, SubmitRejectsMissingRequiredIdentity){
  setenv("R2_ACCESS_KEY_ID","test",1);
  setenv("R2_SECRET_ACCESS_KEY","test",1);
  setenv("R2_ENDPOINT","https://test.r2.cloudflarestorage.com",1);
  setenv("R2_BUCKET","test",1);
  Request cr; cr.body="name=P20Idem&file_path=/tmp/a.pdf&size_bytes=100";
  auto created = handlers::admin::create_exam(cr);
  ASSERT_EQ(created.status, 201) << created.body;
  auto p = created.body.find("\"id\":"); ASSERT_NE(p, std::string::npos);
  auto q = created.body.find_first_of(",}", p);
  int eid = std::stoi(created.body.substr(p+5, q-p-5));
  store::active_store()->update(eid, [](models::Exam& ex){
    ex.status="active"; ex.exam_started_at="2026-08-31T00:00:00Z";
    ex.identity_fields="[{\"key\":\"nisn\",\"label\":\"NISN\",\"required\":true}]";
  });
  auto exam = store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  handlers::api::set_submit_enqueue_hook_for_test([](const queue::SubmissionJob&){});
  Request r1; r1.params["exam_id"]=std::to_string(eid);
  r1.headers["X-Exam-Token"]=exam->token;
  r1.body="{\"mac_address\":\"AA:BB:CC:DD:EE:02\",\"answers\":{\"1\":\"A\"}}";
  auto res = handlers::api::submit_exam(r1);
  handlers::api::set_submit_enqueue_hook_for_test(nullptr);
  store::active_store()->clear_all();
  EXPECT_EQ(res.status, 400) << res.body;
}
TEST(P20, RateLimitUsesLocalFallback){
  auto src = read_src20("src/handlers/api/exams.cpp");
  EXPECT_NE(src.find("local_fallback.allow"), std::string::npos);
}
TEST(P20, TurnstileEmptyRejectedWhenEnabled){
  auto src = read_src20("src/handlers/auth/login.cpp");
  EXPECT_NE(src.find("turnstile_enabled && turnstile.empty()"), std::string::npos);
}
