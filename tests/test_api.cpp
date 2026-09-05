#include <gtest/gtest.h>
#include "handlers/api/exams.hpp"
#include "handlers/admin/exams.hpp"
#include "http/router.hpp"
#include "http/router_full.hpp"
#include "config/config.hpp"
#include "store/exam_store.hpp"
#include "store/exam_store_memory.hpp"
using namespace examvan;

/* Helper: buat exam nyata + start (parity Go → token hanya valid bila exam
 * aktif & sudah dimulai). Mengembalikan token permanen. */
static void prepare_started_exam(std::string& out_token, int& out_id){
  examvan::handlers::admin::clear_exams_for_testing();
  setenv("R2_ACCESS_KEY_ID","test",1);
  setenv("R2_SECRET_ACCESS_KEY","test",1);
  setenv("R2_ENDPOINT","https://test.r2.cloudflarestorage.com",1);
  setenv("R2_BUCKET","test",1);
  Request req; req.body="name=ActiveTokenExam&file_path=/tmp/a.pdf&size_bytes=100";
  auto res=examvan::handlers::admin::create_exam(req);
  ASSERT_EQ(res.status,201) << res.body;
  std::string body=res.body;
  auto extract=[&](const std::string& k)->std::string{
    std::string needle="\""+k+"\":"; size_t p=body.find(needle);
    if(p==std::string::npos) return "";
    size_t s=body.find_first_not_of(" \t\r\n",p+needle.size());
    if(s==std::string::npos) return "";
    if(body[s]=='"'){ size_t e=s+1; while(e<body.size()){ if(body[e]=='\\'){e+=2;continue;} if(body[e]=='"')break; e++; } return body.substr(s+1,e-s-1); }
    size_t e=body.find_first_of(",}",s); return body.substr(s,e-s);
  };
  out_token=extract("token");
  out_id=std::stoi(extract("id"));
  // start: status active + exam_started_at (Go parity)
  examvan::store::active_store()->update(out_id, [](examvan::models::Exam& e){
    e.status="active";
    e.exam_started_at="2026-08-31T00:00:00Z";
  });
}

TEST(Api, Health) {
  Request req; req.method="GET"; req.path="/api/health";
  auto res=handlers::api::health(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("required_app_version"), std::string::npos);
}

TEST(Api, VersionGateGoSemantics) {
  Request req; req.method="GET"; req.path="/api/exams";
  req.headers["X-App-Version"]="1.0.0";
  /* required kosong (fresh DB) → Go mengizinkan meski versi tua */
  EXPECT_EQ(handlers::api::list_exams(req).status,200);
  req.headers["X-App-Version"]="2.7.2";
  EXPECT_EQ(handlers::api::list_exams(req).status,200);
  /* tanpa header (client web) → izinkan */
  req.headers.erase("X-App-Version");
  EXPECT_EQ(handlers::api::list_exams(req).status,200);
}

TEST(Api, ListExamsReadsActiveStore) {
  examvan::handlers::admin::clear_exams_for_testing();
  setenv("R2_ACCESS_KEY_ID","test",1);
  setenv("R2_SECRET_ACCESS_KEY","test",1);
  setenv("R2_ENDPOINT","https://test.r2.cloudflarestorage.com",1);
  setenv("R2_BUCKET","test",1);
  Request create; create.method="POST";
  create.body="name=PersistedListExam&file_path=/tmp/a.pdf&size_bytes=100";
  auto created=examvan::handlers::admin::create_exam(create);
  ASSERT_EQ(created.status,201) << created.body;

  Request list; list.method="GET"; list.path="/api/exams";
  auto response=handlers::api::list_exams(list);
  ASSERT_EQ(response.status,200) << response.body;
  EXPECT_NE(response.body.find("PersistedListExam"),std::string::npos);
  EXPECT_NE(response.body.find("\"total\":1"),std::string::npos);
}

TEST(Api, ExamByTokenValid) {
  std::string perm; int eid=0;
  prepare_started_exam(perm, eid);
  ASSERT_FALSE(perm.empty());
  // permanent token harus match karena mode default dynamic + exam started + active
  Request req; req.params["token"]=perm;
  EXPECT_EQ(handlers::api::exam_by_token(req).status,200);
  // token tidak ada di store harus 404
  Request req2; req2.params["token"]="NOTEXIST";
  EXPECT_EQ(handlers::api::exam_by_token(req2).status,404);
}

TEST(Api, SubmitQueued) {
  // Kontrak baru: submit untuk exam yang valid & sudah dimulai → 202 queued.
  std::string perm; int eid=0;
  prepare_started_exam(perm, eid);
  ASSERT_GT(eid,0);
  auto exam=examvan::store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  Request req; req.params["exam_id"]=std::to_string(eid);
  req.headers["X-Exam-Token"]=exam->token;
  EXPECT_EQ(handlers::api::submit_exam(req).status,202);
  // Submit tanpa exam_id → 400 (bukan 202 sukses palsu).
  Request bad; EXPECT_EQ(handlers::api::submit_exam(bad).status,400);
}

TEST(Api, FullRouterHas40Routes) {
  Config cfg; Router r; register_full_routes(r,cfg);
  EXPECT_GE(r.routes().size(), 30u);
  Request req; req.method="GET"; req.path="/api/health";
  EXPECT_EQ(r.dispatch(req).status,200);
  req.path="/api/nonexistent/xyz/missing"; EXPECT_EQ(r.dispatch(req).status,404);
}

TEST(Api, PresignPdfRedirect) {
  using examvan::handlers::api::set_device_approved_hook_for_test;
  set_device_approved_hook_for_test(nullptr);
  // C2: tanpa token → 401; tanpa device approved → 403; exam tidak ada → 404.
  Request missing; missing.params["exam_id"]="99999";
  EXPECT_EQ(handlers::api::exam_pdf(missing).status,404);
  // Exam valid + token + device approved → 302 presigned (bukan placeholder).
  examvan::handlers::admin::clear_exams_for_testing();
  setenv("R2_ACCESS_KEY_ID","test",1);
  setenv("R2_SECRET_ACCESS_KEY","test",1);
  setenv("R2_ENDPOINT","https://test.r2.cloudflarestorage.com",1);
  setenv("R2_BUCKET","test",1);
  Request cr; cr.body="name=PdfRedirect&file_path=soal.pdf&size_bytes=100";
  auto created=examvan::handlers::admin::create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  auto p=created.body.find("\"id\":");
  ASSERT_NE(p,std::string::npos);
  p+=5; auto e=created.body.find_first_of(",}",p);
  std::string id=created.body.substr(p,e-p);
  int eid=std::stoi(id);
  // Aktifkan + mulai (token hanya valid bila exam active & started).
  examvan::store::active_store()->update(eid, [](examvan::models::Exam& ex){
    ex.status="active"; ex.exam_started_at="2026-08-31T00:00:00Z";
  });
  auto exam=examvan::store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  // Tanpa token → 401.
  Request no_tok; no_tok.params["exam_id"]=id;
  EXPECT_EQ(handlers::api::exam_pdf(no_tok).status,401);
  // Token ada tapi device belum approved → 403.
  Request unauth; unauth.params["exam_id"]=id;
  unauth.headers["X-Exam-Token"]=exam->token;
  EXPECT_EQ(handlers::api::exam_pdf(unauth).status,403);
  // Token + device approved → 302 ke presigned URL asli.
  set_device_approved_hook_for_test([](int, const std::string& mac){ return mac=="AA:BB:CC:DD:EE:FF"; });
  Request ok; ok.params["exam_id"]=id;
  ok.headers["X-Exam-Token"]=exam->token;
  ok.headers["X-Device-Id"]="AA:BB:CC:DD:EE:FF";
  auto res=handlers::api::exam_pdf(ok);
  EXPECT_EQ(res.status,302) << res.body;
  EXPECT_EQ(res.headers["Location"].find("r2.example.com"), std::string::npos) << "stub placeholder r2.example.com harus hilang: " << res.headers["Location"];
  EXPECT_NE(res.headers["Location"].find("X-Amz-Signature="), std::string::npos) << res.headers["Location"];
  set_device_approved_hook_for_test(nullptr);
}
