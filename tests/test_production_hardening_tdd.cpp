#include <gtest/gtest.h>
#include "handlers/admin/exams.hpp"
#include "handlers/api/exams.hpp"
#include "handlers/r2/r2.hpp"
#include "config/config.hpp"
#include "helpers/utils.hpp"
#include "http/router.hpp"
#include "http/router_full.hpp"
#include "queue/submission_queue.hpp"
#include "store/exam_store.hpp"
#include "store/exam_store_memory.hpp"
#include <cstdlib>
#include <string>
#include <vector>
using namespace examvan;
using namespace examvan::handlers::admin;
using namespace examvan::handlers::api;

/* ======================================================================
 * Production hardening — TDD (K1: R2 fail-closed, K2: exam_pdf presigned,
 * K3: delete removes R2 object, questions route, stub endpoint validation).
 *
 * Kontrak baru (fail-closed):
 * - R2Client::upload/remove TIDAK boleh sukses palsu. Endpoint non-R2,
 *   bucket kosong, atau tanpa libcurl = gagal (false). Satu-satunya jalan
 *   sukses tanpa R2 nyata adalah opt-in eksplisit EXAMVAN_R2_TESTMODE=1.
 * - create_exam GAGAL (502) bila upload PDF gagal (strict default).
 * - delete_exam harus menghapus object R2 (bukan hanya baris DB).
 * - Endpoint alur siswa tidak boleh sukses palsu: exam harus ada / dimulai.
 * ====================================================================== */

// Helper: multipart body dengan satu field name + satu file PDF valid.
static std::string multipart_pdf(const std::string& boundary, const std::string& filename, const std::string& content){
  std::string b="--"+boundary+"\r\n";
  b+="Content-Disposition: form-data; name=\"name\"\r\n\r\n";
  b+="Ujian R2\r\n";
  b+="--"+boundary+"\r\n";
  b+="Content-Disposition: form-data; name=\"pdf_file\"; filename=\""+filename+"\"\r\n";
  b+="Content-Type: application/pdf\r\n\r\n";
  b+=content+"\r\n";
  b+="--"+boundary+"--\r\n";
  return b;
}
static void set_r2_endpoint(const std::string& endpoint){
  setenv("R2_ACCESS_KEY_ID","k",1);
  setenv("R2_SECRET_ACCESS_KEY","s",1);
  setenv("R2_ENDPOINT",endpoint.c_str(),1);
  setenv("R2_BUCKET","examvan-pdfs",1);
}
static void reset_r2_flags(){
  setenv("EXAMVAN_R2_TESTMODE","",1);
  setenv("EXAMVAN_R2_STRICT","",1);
}
// Ekstrak field string/angka sederhana dari JSON respons create.
static std::string json_field(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\":";
  size_t p=body.find(needle);
  if(p==std::string::npos) return "";
  size_t s=body.find_first_not_of(" \t\r\n",p+needle.size());
  if(s==std::string::npos) return "";
  if(body[s]=='"'){
    size_t e=s+1; while(e<body.size()){ if(body[e]=='\\'){e+=2;continue;} if(body[e]=='"')break; e++; }
    return body.substr(s+1,e-s-1);
  }
  size_t e=body.find_first_of(",}",s);
  return body.substr(s,e-s);
}
// Buat exam + tandai active & started; kembalikan id.
static int create_started_exam_id(){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request cr; cr.body="name=StartedExam&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  if(created.status!=201) return -1;
  int id=std::stoi(json_field(created.body,"id"));
  examvan::store::active_store()->update(id,[](examvan::models::Exam& e){
    e.status="active";
    e.exam_started_at="2026-08-31T00:00:00Z";
  });
  return id;
}

// ----------------------------------------------------------------------
// K1 — R2 upload fail-closed
// ----------------------------------------------------------------------

TEST(ProductionHardening, R2Upload_FailsClosedOnNonR2Endpoint){
  clear_exams_for_testing();
  reset_r2_flags();
  set_r2_endpoint("https://wrong.example.com");
  std::string boundary="----K1";
  Request req; req.body=multipart_pdf(boundary,"soal.pdf","%PDF-1.4 fake\n%%EOF\n");
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,502) << res.body;
  EXPECT_NE(res.body.find("UPLOAD_FAILED"), std::string::npos) << res.body;
  EXPECT_EQ(examvan::store::active_store()->count(),0u)
    << "exam TIDAK boleh terdaftar bila PDF gagal diupload";
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
}

TEST(ProductionHardening, R2Upload_NonR2EndpointWithoutTestMode_FailsClosedUnit){
  reset_r2_flags();
  r2::R2Config rc{"k","s","https://wrong.example.com","examvan-pdfs"};
  r2::R2Client client{rc};
  EXPECT_FALSE(client.upload("exams/1/soal.pdf","%PDF-1.4\n%%EOF\n"));
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  EXPECT_TRUE(client.upload("exams/1/soal.pdf","%PDF-1.4\n%%EOF\n"));
  reset_r2_flags();
}

TEST(ProductionHardening, R2Upload_ExplicitTestModeOptIn){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  setenv("EXAMVAN_R2_STRICT","",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  std::string boundary="----K1b";
  Request req; req.body=multipart_pdf(boundary,"soal.pdf","%PDF-1.4 fake\n%%EOF\n");
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,201) << res.body;
  reset_r2_flags();
}

TEST(ProductionHardening, R2Upload_BucketRequired){
  reset_r2_flags();
  r2::R2Config rc{"k","s","https://test.r2.cloudflarestorage.com",""};
  r2::R2Client client{rc};
  EXPECT_FALSE(client.upload("exams/1/soal.pdf","%PDF-1.4\n%%EOF\n"));
}

// ----------------------------------------------------------------------
// K2 — GET /api/exams/:exam_id/pdf harus redirect ke presigned URL R2 asli
// ----------------------------------------------------------------------

TEST(ProductionHardening, ExamPdf_MissingExam404){
  clear_exams_for_testing();
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request req; req.params["exam_id"]="99999";
  auto res=exam_pdf(req);
  EXPECT_EQ(res.status,404);
}

TEST(ProductionHardening, ExamPdf_RedirectsToPresignedUrl){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request cr; cr.body="name=PdfExam&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  std::string id=json_field(created.body,"id");
  Request req; req.params["exam_id"]=id;
  auto res=exam_pdf(req);
  EXPECT_EQ(res.status,302);
  std::string loc=res.headers["Location"];
  EXPECT_NE(loc.find("https://test.r2.cloudflarestorage.com"), std::string::npos) << loc;
  EXPECT_NE(loc.find("exams/"+id+"/soal.pdf"), std::string::npos) << loc;
  EXPECT_NE(loc.find("X-Amz-Signature="), std::string::npos) << loc;
  reset_r2_flags();
}

TEST(ProductionHardening, ExamPdf_R2NotConfigured503){
  clear_exams_for_testing();
  setenv("R2_ACCESS_KEY_ID","",1);
  setenv("R2_SECRET_ACCESS_KEY","",1);
  setenv("R2_ENDPOINT","",1);
  setenv("R2_BUCKET","",1);
  reset_r2_flags();
  Request cr; cr.body="name=PdfNoR2&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  std::string id=json_field(created.body,"id");
  Request req; req.params["exam_id"]=id;
  auto res=exam_pdf(req);
  EXPECT_EQ(res.status,503);
  EXPECT_NE(res.body.find("R2_NOT_CONFIGURED"), std::string::npos) << res.body;
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
}

// ----------------------------------------------------------------------
// K3 — delete_exam harus menghapus object R2 (bukan hanya baris store)
// ----------------------------------------------------------------------

TEST(ProductionHardening, DeleteExam_RemovesR2Object){
  clear_exams_for_testing();
  std::vector<std::pair<std::string,std::string>> calls;
  set_upload_mock_for_test([&](const std::string& k, const std::string& d){
    calls.emplace_back(k,d);
  });
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  std::string boundary="----K3";
  Request cr; cr.body=multipart_pdf(boundary,"soal.pdf","%PDF-1.4 fake\n%%EOF\n");
  cr.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  ASSERT_FALSE(calls.empty()) << "upload mock harus terpanggil saat create";
  std::string upload_key=calls.back().first;
  EXPECT_NE(upload_key.find("exams/"), std::string::npos);
  std::string id=json_field(created.body,"id");

  Request rd; rd.params["id"]=id;
  auto res=delete_exam(rd);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_EQ(examvan::store::active_store()->count(),0u);
  bool removed=false;
  for(auto& kv: calls) if(kv.first==upload_key && kv.second.empty()) removed=true;
  EXPECT_TRUE(removed) << "delete harus memanggil R2 remove untuk key " << upload_key;
}

TEST(ProductionHardening, DeleteExam_R2NotConfigured_Refuses){
  clear_exams_for_testing();
  setenv("R2_ACCESS_KEY_ID","",1);
  setenv("R2_SECRET_ACCESS_KEY","",1);
  setenv("R2_ENDPOINT","",1);
  setenv("R2_BUCKET","",1);
  reset_r2_flags();
  Request cr; cr.body="name=DelNoR2&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  std::string id=json_field(created.body,"id");
  Request rd; rd.params["id"]=id;
  auto res=delete_exam(rd);
  EXPECT_EQ(res.status,503) << res.body;
  EXPECT_NE(res.body.find("R2_NOT_CONFIGURED"), std::string::npos) << res.body;
  EXPECT_EQ(examvan::store::active_store()->count(),1u)
    << "exam TIDAK boleh terhapus bila object R2 tidak bisa dibersihkan";
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
}

// ----------------------------------------------------------------------
// Route questions — GET/POST /admin/api/exams/:exam_id/questions
// ----------------------------------------------------------------------

TEST(ProductionHardening, Questions_Get_EmptyForNewExam){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request gq; gq.params["exam_id"]=std::to_string(id);
  auto res=get_exam_questions(gq);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"questions\":[]"), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"token\""), std::string::npos) << res.body;
}

TEST(ProductionHardening, Questions_SaveThenGet_RoundTrip){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":[{\"number\":1,\"type\":\"single_choice\",\"weight\":1.0,\"key\":\"A\",\"choices\":[\"A\",\"B\",\"C\"]}],"
    "\"security_level\":\"high\",\"strict_mode\":true,"
    "\"identity_fields\":[{\"key\":\"student_name\",\"label\":\"Nama\",\"required\":true}],"
    "\"panel_color\":\"#112233\",\"start_time\":\"2026-09-01 08:00\",\"end_time\":\"2026-09-01 09:00\","
    "\"congrats_message\":\"Selamat!\"}";
  auto saved=save_exam_questions(sq);
  EXPECT_EQ(saved.status,200) << saved.body;
  EXPECT_NE(saved.body.find("\"success\":true"), std::string::npos) << saved.body;

  Request gq; gq.params["exam_id"]=std::to_string(id);
  auto res=get_exam_questions(gq);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("\"single_choice\""), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"security_level\":\"high\""), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"panel_color\":\"#112233\""), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"congrats_message\":\"Selamat!\""), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"student_name\""), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"questions\":["), std::string::npos) << res.body;
}

TEST(ProductionHardening, Questions_MissingExam404){
  Request gq; gq.params["exam_id"]="99999";
  EXPECT_EQ(get_exam_questions(gq).status,404);
  Request sq; sq.params["exam_id"]="99999"; sq.body="{\"questions\":[]}";
  EXPECT_EQ(save_exam_questions(sq).status,404);
}

TEST(ProductionHardening, Questions_InvalidQuestionsNotArray400){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":\"not-an-array\"}";
  auto res=save_exam_questions(sq);
  EXPECT_EQ(res.status,400) << res.body;
}

TEST(ProductionHardening, Questions_RouteRegistered){
  Config cfg; Router r; register_full_routes(r,cfg);
  bool has=false;
  for(auto& s: r.routes()) if(s.find("questions")!=std::string::npos) has=true;
  EXPECT_TRUE(has) << "route /admin/api/exams/:exam_id/questions harus terdaftar";
}

// ----------------------------------------------------------------------
// Stub endpoints — validasi nyata (bukan sukses palsu)
// ----------------------------------------------------------------------

TEST(ProductionHardening, SubmitExam_ValidStartedExam_202WithHook){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  bool hooked=false; int hooked_exam=0; std::string hooked_answers;
  set_submit_enqueue_hook_for_test([&](const queue::SubmissionJob& j){
    hooked=true; hooked_exam=j.exam_id;
    auto it=j.answers.find("1"); if(it!=j.answers.end()) hooked_answers=it->second;
  });
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["Content-Type"]="application/json";
  rq.body="{\"student_name\":\"Ani\",\"answers\":{\"1\":\"A\"}}";
  auto res=submit_exam(rq);
  EXPECT_EQ(res.status,202) << res.body;
  EXPECT_NE(res.body.find("\"queued\""), std::string::npos) << res.body;
  EXPECT_TRUE(hooked) << "submit harus men-enqueue job";
  EXPECT_EQ(hooked_exam, id);
  EXPECT_EQ(hooked_answers, "A");
  set_submit_enqueue_hook_for_test(nullptr);
}

TEST(ProductionHardening, SubmitExam_MissingExam404){
  Request rq; rq.params["exam_id"]="99999";
  auto res=submit_exam(rq);
  EXPECT_EQ(res.status,404);
}

TEST(ProductionHardening, SubmitExam_NotStarted403){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request cr; cr.body="name=NotStarted&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  std::string id=json_field(created.body,"id");
  Request rq; rq.params["exam_id"]=id;
  auto res=submit_exam(rq);
  EXPECT_EQ(res.status,403) << res.body;
  EXPECT_NE(res.body.find("exam not started"), std::string::npos) << res.body;
}

TEST(ProductionHardening, ExamResult_MissingExam404){
  Request rq; rq.params["exam_id"]="99999";
  EXPECT_EQ(exam_result(rq).status,404);
}

TEST(ProductionHardening, ExamResult_ValidExam200){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request rq; rq.params["exam_id"]=std::to_string(id);
  auto res=exam_result(rq);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("\"exam_id\":"+std::to_string(id)), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"score\""), std::string::npos) << res.body;
}

TEST(ProductionHardening, AccessLog_MissingExam404){
  Request rq; rq.params["exam_id"]="99999";
  EXPECT_EQ(access_log(rq).status,404);
}

TEST(ProductionHardening, AccessLog_ValidExam200){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request rq; rq.params["exam_id"]=std::to_string(id);
  auto res=access_log(rq);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("\"logged\":true"), std::string::npos) << res.body;
}

TEST(ProductionHardening, CompleteExam_MissingExam404){
  Request rq; rq.params["exam_id"]="99999";
  EXPECT_EQ(complete_exam(rq).status,404);
}

TEST(ProductionHardening, CompleteExam_ValidExam200){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request rq; rq.params["exam_id"]=std::to_string(id);
  auto res=complete_exam(rq);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("\"completed\":true"), std::string::npos) << res.body;
}

TEST(ProductionHardening, RequestApproval_NoToken400){
  Request rq; rq.body="";
  auto res=request_approval(rq);
  EXPECT_EQ(res.status,400) << res.body;
  EXPECT_NE(res.body.find("token"), std::string::npos) << res.body;
}

TEST(ProductionHardening, RequestApproval_UnknownToken404){
  Request rq; rq.body="token=NOTEXIST";
  auto res=request_approval(rq);
  EXPECT_EQ(res.status,404) << res.body;
}

TEST(ProductionHardening, RequestApproval_ValidToken200){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.body="token="+exam->token;
  auto res=request_approval(rq);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("\"pending\""), std::string::npos) << res.body;
}