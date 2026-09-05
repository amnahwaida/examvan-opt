#include <gtest/gtest.h>
#include "handlers/admin/exams.hpp"
#include "handlers/api/exams.hpp"
#include "handlers/r2/r2.hpp"
#include "config/config.hpp"
#include "helpers/utils.hpp"
#include "http/router.hpp"
#include "http/router_full.hpp"
#include "queue/submission_queue.hpp"
#include "jobs/jobs.hpp"
#include "middleware/scoring.hpp"
#include "store/exam_store.hpp"
#include "store/exam_store_memory.hpp"
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include <chrono>
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
static std::string read_source_file(const std::string& path){
  std::ifstream f(path);
  std::ostringstream ss;
  if(f) ss << f.rdbuf();
  return ss.str();
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

TEST(ProductionHardening, R2Verify_FailsClosedUnit){
  reset_r2_flags();
  r2::R2Config rc{"k","s","https://wrong.example.com","examvan-pdfs"};
  r2::R2Client client{rc};
  EXPECT_FALSE(client.verify("exams/1/soal.pdf"));
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  EXPECT_TRUE(client.verify("exams/1/soal.pdf"));
  reset_r2_flags();
}

TEST(ProductionHardening, R2Upload_VerifyPassesInTestMode){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  std::string boundary="----V1";
  Request req; req.body=multipart_pdf(boundary,"soal.pdf","%PDF-1.4 fake\n%%EOF\n");
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,201) << res.body;
  reset_r2_flags();
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
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["Content-Type"]="application/json";
  rq.headers["X-Exam-Token"]=exam->token;
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

TEST(ProductionHardening, SubmitExam_NoToken401){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.body="{\"student_name\":\"Ani\"}"; // tanpa token
  auto res=submit_exam(rq);
  EXPECT_EQ(res.status,401) << res.body;
  EXPECT_NE(res.body.find("Token tidak disertakan"), std::string::npos) << res.body;
}

TEST(ProductionHardening, SubmitExam_WrongToken404){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]="WRONGTOKEN";
  rq.body="{\"student_name\":\"Ani\",\"mac_address\":\"aa:bb\"}";
  auto res=submit_exam(rq);
  EXPECT_EQ(res.status,404) << res.body;
  EXPECT_NE(res.body.find("Ujian tidak ditemukan"), std::string::npos) << res.body;
}

TEST(ProductionHardening, SubmitExam_Ended403UnlessApproved){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  examvan::store::active_store()->update(id,[](examvan::models::Exam& e){
    e.end_time="2020-01-01T00:00:00Z"; // sudah lewat + grace 60s
  });
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]=exam->token;
  rq.body="{\"student_name\":\"Ani\",\"mac_address\":\"aa:bb\"}";
  auto res=submit_exam(rq);
  // Tanpa DB, device_approved=false → 403 (paritas Go: hanya device approved
  // boleh recovery-resubmit lewat deadline).
  EXPECT_EQ(res.status,403) << res.body;
  EXPECT_NE(res.body.find("Waktu ujian telah berakhir"), std::string::npos) << res.body;
}

TEST(ProductionHardening, SubmitExam_TokenValidationGo){
  auto c=read_source_file("src/handlers/api/exams.cpp");
  EXPECT_NE(c.find("Token tidak disertakan"), std::string::npos) << "401 token kosong (Go)";
  EXPECT_NE(c.find("Waktu ujian telah berakhir"), std::string::npos) << "403 ended (Go)";
  EXPECT_NE(c.find("exam_approvals"), std::string::npos) << "approved lookup (Go)";
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
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]=exam->token;
  auto res=access_log(rq);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("\"logged\":true"), std::string::npos) << res.body;
}

TEST(ProductionHardening, AccessLog_WrongToken404){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]="WRONGTOKEN";
  auto res=access_log(rq);
  EXPECT_EQ(res.status,404) << res.body;
}

TEST(ProductionHardening, AccessLog_TokenViaQuery200){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.query="token="+exam->token;
  auto res=access_log(rq);
  EXPECT_EQ(res.status,200) << res.body;
}

TEST(ProductionHardening, AccessLog_NotStarted404){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request cr; cr.body="name=NotStartedLog&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  int id=std::stoi(json_field(created.body,"id"));
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]=exam->token;
  auto res=access_log(rq);
  EXPECT_EQ(res.status,404) << res.body; // Go: exam tidak aktif/belum start → 404
}

TEST(ProductionHardening, AccessLog_ScheduleEnded403){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  examvan::store::active_store()->update(id,[](examvan::models::Exam& e){
    e.end_time="2020-01-01T00:00:00Z"; // sudah lewat + grace 60s
  });
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]=exam->token;
  auto res=access_log(rq);
  EXPECT_EQ(res.status,403) << res.body;
  EXPECT_NE(res.body.find("Waktu ujian telah berakhir"), std::string::npos) << res.body;
}

TEST(ProductionHardening, AccessLog_TokenValidationGo){
  auto c=read_source_file("src/handlers/api/exams.cpp");
  EXPECT_NE(c.find("X-Exam-Token"), std::string::npos)
    << "token dari header X-Exam-Token (paritas Go)";
  EXPECT_NE(c.find("exam_approvals"), std::string::npos)
    << "device approved lookup via exam_approvals";
  EXPECT_NE(c.find("Waktu ujian telah berakhir"), std::string::npos) << "403 schedule-ended (Go)";
  EXPECT_NE(c.find("seconds(60)"), std::string::npos) << "grace SubmissionGraceEnd 60s";
}

// ----------------------------------------------------------------------
// Kontrak Go access_log (handlers/api/exams.go) — event & identity_data
// ----------------------------------------------------------------------

TEST(ProductionHardening, AccessLog_InvalidEvent400){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["Content-Type"]="application/json";
  rq.body="{\"event\":\"bogus\"}";
  auto res=access_log(rq);
  EXPECT_EQ(res.status,400) << res.body;
  EXPECT_NE(res.body.find("Event tidak valid"), std::string::npos) << res.body;
}

TEST(ProductionHardening, AccessLog_EmptyEventOk200){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]=exam->token;
  rq.body="{\"student_name\":\"Ani\"}"; // tanpa event → default heartbeat (Go)
  auto res=access_log(rq);
  EXPECT_EQ(res.status,200) << res.body;
}

TEST(ProductionHardening, AccessLog_LoginLogoutHeartbeatEventsOk200){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  for(auto ev: {"login","heartbeat","logout"}){
    Request rq; rq.params["exam_id"]=std::to_string(id);
    rq.headers["X-Exam-Token"]=exam->token;
    rq.headers["Content-Type"]="application/json";
    rq.body=std::string("{\"event\":\"")+ev+"\"}";
    EXPECT_EQ(access_log(rq).status,200) << ev;
  }
}

TEST(ProductionHardening, AccessLog_EventContractGo){
  auto c=read_source_file("src/handlers/api/exams.cpp");
  EXPECT_NE(c.find("if(event.empty()) event=\"heartbeat\";"), std::string::npos)
    << "Go: event kosong → heartbeat (bukan login)";
  EXPECT_NE(c.find("event!=\"login\" && event!=\"heartbeat\" && event!=\"logout\""), std::string::npos)
    << "Go: hanya login/heartbeat/logout yang valid";
  EXPECT_EQ(c.find("event=\"login\";"), std::string::npos) << "default lama 'login' harus hilang";
}

TEST(ProductionHardening, AccessLog_IdentityDataSanitizedGo){
  auto c=read_source_file("src/handlers/api/exams.cpp");
  EXPECT_NE(c.find("sanitize_identity_map_json"), std::string::npos)
    << "identity_data harus di-sanitize per nilai (paritas Go sanitizeMap)";
  EXPECT_NE(c.find("sanitize_student_input"), std::string::npos);
}

TEST(ProductionHardening, AccessLog_MacSanitizedGo){
  auto c=read_source_file("src/handlers/api/exams.cpp");
  EXPECT_NE(c.find("sanitize_mac_like_go"), std::string::npos)
    << "mac harus di-sanitize (paritas Go sanitizeMAC)";
  EXPECT_NE(c.find("\"unknown\""), std::string::npos) << "mac kosong → 'unknown'";
}

TEST(ProductionHardening, CompleteExam_MissingExam404){
  Request rq; rq.params["exam_id"]="99999";
  rq.headers["X-Exam-Token"]="TOKEN";
  rq.body="mac_address=aa:bb";
  EXPECT_EQ(complete_exam(rq).status,404);
}

TEST(ProductionHardening, CompleteExam_NoToken401){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.body="mac_address=aa:bb";
  auto res=complete_exam(rq);
  EXPECT_EQ(res.status,401) << res.body;
  EXPECT_NE(res.body.find("Token tidak disertakan"), std::string::npos) << res.body;
}

TEST(ProductionHardening, CompleteExam_NoMac400){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]=exam->token;
  auto res=complete_exam(rq);
  EXPECT_EQ(res.status,400) << res.body;
  EXPECT_NE(res.body.find("MAC address diperlukan"), std::string::npos) << res.body;
}

TEST(ProductionHardening, CompleteExam_WrongToken404){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]="WRONGTOKEN";
  rq.body="mac_address=aa:bb";
  auto res=complete_exam(rq);
  EXPECT_EQ(res.status,404) << res.body;
}

TEST(ProductionHardening, CompleteExam_NotActive404){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request cr; cr.body="name=NotActiveComp&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  int id=std::stoi(json_field(created.body,"id"));
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]=exam->token;
  rq.body="mac_address=aa:bb";
  auto res=complete_exam(rq);
  EXPECT_EQ(res.status,404) << res.body;
}

TEST(ProductionHardening, CompleteExam_ValidExam200){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]=exam->token;
  rq.body="mac_address=aa:bb";
  auto res=complete_exam(rq);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("\"completed\":true"), std::string::npos) << res.body;
}

TEST(ProductionHardening, CompleteExam_ParityGo){
  auto c=read_source_file("src/handlers/api/exams.cpp");
  EXPECT_NE(c.find("Token tidak disertakan"), std::string::npos) << "401 token kosong (Go)";
  EXPECT_NE(c.find("MAC address diperlukan"), std::string::npos) << "400 tanpa MAC (Go)";
  EXPECT_NE(c.find("\"heartbeat:\""), std::string::npos) << "DEL heartbeat key (presence offline)";
  EXPECT_NE(c.find("DEL"), std::string::npos);
}

TEST(ProductionHardening, RequestApproval_NoToken400){
  Request rq; rq.body="";
  auto res=request_approval(rq);
  EXPECT_EQ(res.status,400) << res.body;
  EXPECT_NE(res.body.find("exam id"), std::string::npos) << res.body;
}

TEST(ProductionHardening, RequestApproval_UnknownToken401){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request rq; rq.body="exam_id="+std::to_string(id)+"&mac_address=aa:bb:cc&token=NOTEXIST";
  auto res=request_approval(rq);
  EXPECT_EQ(res.status,401) << res.body;
  EXPECT_NE(res.body.find("Token tidak valid"), std::string::npos) << res.body;
}

TEST(ProductionHardening, RequestApproval_MissingMac400){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.body="exam_id="+std::to_string(id)+"&token="+exam->token; // tanpa mac
  auto res=request_approval(rq);
  EXPECT_EQ(res.status,400) << res.body;
  EXPECT_NE(res.body.find("MAC address diperlukan"), std::string::npos) << res.body;
}

TEST(ProductionHardening, RequestApproval_ValidToken200){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq; rq.body="exam_id="+std::to_string(id)+"&mac_address=aa:bb:cc&student_name=Ani&token="+exam->token;
  auto res=request_approval(rq);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("\"pending\""), std::string::npos) << res.body;
}

TEST(ProductionHardening, RequestApproval_PersistsGo){
  auto c=read_source_file("src/handlers/api/exams.cpp");
  EXPECT_NE(c.find("INSERT INTO exam_approvals"), std::string::npos)
    << "request_approval harus benar-benar INSERT ke exam_approvals (bukan stub)";
  EXPECT_NE(c.find("ON CONFLICT (exam_id, mac_address)"), std::string::npos);
  EXPECT_NE(c.find("RETURNING status"), std::string::npos);
}

TEST(ProductionHardening, RequestApproval_NumericJsonExamIdAccepted){
  // Klien nyata (app) mengirim "exam_id":<angka> — bukan string. json_string_field
  // hanya membaca nilai ber-quote → 400 "exam id required" (bug ditemukan smoke test).
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq;
  rq.body="{\"exam_id\":"+std::to_string(id)+",\"mac_address\":\"aa:bb:cc\",\"token\":\""+exam->token+"\"}";
  auto res=request_approval(rq);
  EXPECT_EQ(res.status,200) << res.body;
}

TEST(ProductionHardening, RequestApproval_BooleanResetAccepted){
  // Go memakai field reset bool (JSON true/false tanpa quote) — jangan 400/fallback.
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  Request rq;
  rq.body="{\"exam_id\":"+std::to_string(id)+",\"mac_address\":\"aa:bb:cc\",\"token\":\""+exam->token+"\",\"reset\":true}";
  auto res=request_approval(rq);
  EXPECT_EQ(res.status,200) << res.body;
}

// ----------------------------------------------------------------------
// Health — key status tidak boleh dobel ("healthy" + "ok" duplikat)
// ----------------------------------------------------------------------

TEST(ProductionHardening, Health_SingleStatusKey){
  Request req; req.method="GET"; req.path="/api/health";
  auto res=health(req);
  EXPECT_EQ(res.status,200);
  size_t count=0, pos=0;
  while((pos=res.body.find("\"status\":",pos))!=std::string::npos){ count++; pos+=9; }
  EXPECT_EQ(count,1u) << "health hanya boleh punya SATU key status: " << res.body;
  EXPECT_NE(res.body.find("\"status\":\"healthy\""), std::string::npos) << res.body;
}

// ----------------------------------------------------------------------
// Expiry job — tombstone otomatis exam yang kedaluwarsa + purge 30 hari
// ----------------------------------------------------------------------

TEST(ProductionHardening, Expiry_TombstonesExpiredExam){
  clear_exams_for_testing();
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request cr; cr.body="name=Expired&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  int id=std::stoi(json_field(created.body,"id"));
  examvan::store::active_store()->update(id,[](examvan::models::Exam& e){
    e.status="active";
    e.end_time="2020-01-01T00:00:00Z"; // sudah lewat
  });
  examvan::jobs::run_expiry_job();
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  EXPECT_TRUE(exam->tombstoned_at.has_value()) << "exam kedaluwarsa harus di-tombstone";
  // Paritas Go: tombstone = status 'inactive' + tombstoned_at (CHECK constraint
  // exams.status hanya mengizinkan active/inactive — 'deleted' akan ditolak PG).
  EXPECT_EQ(exam->status, "inactive");
}

TEST(ProductionHardening, Expiry_KeepsFutureExam){
  clear_exams_for_testing();
  Request cr; cr.body="name=Future&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  int id=std::stoi(json_field(created.body,"id"));
  examvan::store::active_store()->update(id,[](examvan::models::Exam& e){
    e.status="active";
    e.end_time="2099-01-01T00:00:00Z"; // masih jauh
  });
  examvan::jobs::run_expiry_job();
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  EXPECT_FALSE(exam->tombstoned_at.has_value()) << "exam masa depan tidak boleh di-tombstone";
  EXPECT_EQ(exam->status, "active");
}

TEST(ProductionHardening, Expiry_TombstonesByCreatedAtAge){
  clear_exams_for_testing();
  Request cr; cr.body="name=OldCreated&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  int id=std::stoi(json_field(created.body,"id"));
  // Tanpa end_time: kedaluwarsa = created_at + 14 hari. Buat created_at 30 hari lalu.
  examvan::store::active_store()->update(id,[](examvan::models::Exam& e){
    e.created_at="2020-01-01T00:00:00Z";
  });
  examvan::jobs::run_expiry_job();
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  EXPECT_TRUE(exam->tombstoned_at.has_value()) << "created_at terlalu tua harus di-tombstone";
}

TEST(ProductionHardening, Expiry_DoesNotReTombstoneRecent){
  clear_exams_for_testing();
  Request cr; cr.body="name=Tombstoned&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  int id=std::stoi(json_field(created.body,"id"));
  examvan::store::active_store()->update(id,[](examvan::models::Exam& e){
    e.status="inactive";
    e.tombstoned_at="2026-09-01T00:00:00Z"; // baru, belum 30 hari
  });
  examvan::jobs::run_expiry_job();
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value()) << "tombstone <30 hari tidak boleh di-purge";
  EXPECT_EQ(exam->tombstoned_at.value_or(""), "2026-09-01T00:00:00Z");
}

TEST(ProductionHardening, Expiry_KeepsOldTombstoned){
  clear_exams_for_testing();
  Request cr; cr.body="name=KeepMe&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  int id=std::stoi(json_field(created.body,"id"));
  examvan::store::active_store()->update(id,[](examvan::models::Exam& e){
    e.status="inactive";
    e.tombstoned_at="2020-01-01T00:00:00Z"; // >30 hari
  });
  examvan::jobs::run_expiry_job();
  EXPECT_TRUE(examvan::store::active_store()->get_by_id(id).has_value())
    << "Go TIDAK pernah auto-delete exam (delete hanya eksplisit via delete_exam "
       "yang juga membersihkan R2) — tombstone lama tetap disimpan";
}

// ----------------------------------------------------------------------
// Alur siswa — scoring pakai questions_json exam yang nyata
// ----------------------------------------------------------------------

TEST(ProductionHardening, Scoring_UsesQuestionsJson){
  std::string qjson="[{\"number\":1,\"type\":\"single_choice\",\"weight\":1.0,\"key\":\"A\"},"
    "{\"number\":2,\"type\":\"single_choice\",\"weight\":1.0,\"key\":\"B\"}]";
  std::map<std::string,std::string> benar{{"1","A"},{"2","B"}};
  auto s=examvan::scoring::score_submission_json(qjson, benar);
  ASSERT_TRUE(s.has_value());
  EXPECT_DOUBLE_EQ(*s, 100.0);
  std::map<std::string,std::string> salah{{"1","A"},{"2","C"}};
  auto s2=examvan::scoring::score_submission_json(qjson, salah);
  ASSERT_TRUE(s2.has_value());
  EXPECT_DOUBLE_EQ(*s2, 50.0);
  // Tanpa soal / soal kosong → nullopt (tidak bisa dinilai).
  EXPECT_FALSE(examvan::scoring::score_submission_json("", benar).has_value());
  EXPECT_FALSE(examvan::scoring::score_submission_json("[{\"type\":\"x\"}]", benar).has_value());
}

TEST(ProductionHardening, JobResult_ScoreSerialized){
  queue::JobResult r; r.job_id="j1"; r.success=true; r.score=87.5; r.message="ok";
  auto j=r.to_json();
  EXPECT_NE(j.find("\"score\":87.5"), std::string::npos) << j;
  queue::JobResult r2; r2.job_id="j2"; r2.success=true; r2.message="ok";
  auto j2=r2.to_json();
  EXPECT_NE(j2.find("\"score\":null"), std::string::npos) << j2;
}

// ----------------------------------------------------------------------
// Verifikasi schema Go (webui/internal/database/schema.sql) — nama tabel,
// kolom & status harus cocok dengan produksi (CHECK constraint dll).
// Temuan: submissions TIDAK punya job_id/status/submitted_at; tabel access
// log bernama student_access_logs (bukan access_log); exams.status CHECK
// hanya mengizinkan active/inactive (bukan 'deleted').
// ----------------------------------------------------------------------

TEST(ProductionHardening, Expiry_TombstoneUsesInactiveStatus){
  auto j = read_source_file("src/jobs/jobs.cpp");
  EXPECT_NE(j.find("ex.status=\"inactive\""), std::string::npos)
    << "tombstone harus status='inactive' (CHECK Go: active/inactive)";
  EXPECT_EQ(j.find("ex.status=\"deleted\""), std::string::npos) << j;
}

TEST(ProductionHardening, Expiry_NoAutoPurgeOfExams){
  auto j = read_source_file("src/jobs/jobs.cpp");
  EXPECT_EQ(j.find("DELETE FROM exams"), std::string::npos)
    << "Go tidak pernah auto-delete exam (delete hanya eksplisit via delete_exam)";
}

TEST(ProductionHardening, ApprovalCleanup_UsesExamApprovalsTable){
  auto j = read_source_file("src/jobs/jobs.cpp");
  EXPECT_NE(j.find("exam_approvals"), std::string::npos)
    << "tabel Go bernama exam_approvals, bukan approvals";
  EXPECT_EQ(j.find("DELETE FROM approvals"), std::string::npos) << j;
}

TEST(ProductionHardening, AccessLogRetention_UsesStudentAccessLogsTable){
  auto j = read_source_file("src/jobs/jobs.cpp");
  EXPECT_NE(j.find("student_access_logs"), std::string::npos)
    << "tabel Go bernama student_access_logs, bukan access_log";
  EXPECT_EQ(j.find("DELETE FROM access_log"), std::string::npos) << j;
}

TEST(ProductionHardening, SubmissionsInsert_MatchesGoSchema){
  auto c = read_source_file("src/queue/submission_queue.cpp");
  size_t p = c.find("INSERT INTO submissions");
  ASSERT_NE(p, std::string::npos);
  size_t e = c.find(";", p);
  std::string insert = (e==std::string::npos) ? c.substr(p) : c.substr(p, e-p);
  // Kolom yang ADA di schema Go: exam_id, student_name, exam_number,
  // student_class, answers_json, score, start_time, mac_address, identity_data.
  EXPECT_NE(insert.find("answers_json"), std::string::npos) << insert;
  EXPECT_NE(insert.find("score"), std::string::npos) << insert;
  EXPECT_NE(insert.find("mac_address"), std::string::npos) << insert;
  EXPECT_NE(insert.find("identity_data"), std::string::npos) << insert;
  EXPECT_NE(insert.find("start_time"), std::string::npos) << insert;
  // Kolom yang TIDAK ADA di schema Go (INSERT lama pasti gagal di produksi):
  EXPECT_EQ(insert.find("job_id"), std::string::npos) << insert;
  EXPECT_EQ(insert.find("submitted_at"), std::string::npos) << insert;
  EXPECT_EQ(insert.find("status"), std::string::npos) << insert;
}

TEST(ProductionHardening, AccessLogInsert_MatchesGoSchema){
  auto c = read_source_file("src/handlers/api/exams.cpp");
  size_t p = c.find("INSERT INTO student_access_logs");
  ASSERT_NE(p, std::string::npos) << "harus INSERT ke student_access_logs";
  size_t e = c.find(";", p);
  std::string insert = (e==std::string::npos) ? c.substr(p) : c.substr(p, e-p);
  EXPECT_NE(insert.find("student_identifier"), std::string::npos) << insert;
  EXPECT_NE(insert.find("event"), std::string::npos) << insert;
  EXPECT_NE(insert.find("ip_address"), std::string::npos) << insert;
  EXPECT_NE(insert.find("device_info"), std::string::npos) << insert;
  EXPECT_NE(insert.find("identity_data"), std::string::npos) << insert;
  EXPECT_EQ(c.find("INSERT INTO access_log ("), std::string::npos)
    << "tabel lama access_log tidak ada di schema Go";
}

TEST(ProductionHardening, Queue_JobJsonRoundTrip_FullFields){
  queue::SubmissionJob j;
  j.job_id="e2e1"; j.exam_id=9; j.student_name="Budi"; j.exam_number="01";
  j.student_class="XII-A"; j.start_time="2026-09-01T08:00:00Z";
  j.mac_address="aa:bb"; j.enqueued_at="2026-09-01T08:05:00Z"; j.retries=1;
  j.answers={{"1","A"},{"2","B"}};
  j.identity_data={{"nis","12345"}};
  auto j2=queue::SubmissionJob::from_json(j.to_json());
  ASSERT_TRUE(j2.has_value());
  EXPECT_EQ(j2->job_id, "e2e1");
  EXPECT_EQ(j2->exam_id, 9);
  EXPECT_EQ(j2->student_name, "Budi");
  EXPECT_EQ(j2->exam_number, "01");
  EXPECT_EQ(j2->student_class, "XII-A");
  EXPECT_EQ(j2->mac_address, "aa:bb");
  EXPECT_EQ(j2->start_time, "2026-09-01T08:00:00Z");
  EXPECT_EQ(j2->enqueued_at, "2026-09-01T08:05:00Z");
  EXPECT_EQ(j2->retries, 1);
  ASSERT_EQ(j2->answers.size(), 2u);
  EXPECT_EQ(j2->answers["1"], "A");
  EXPECT_EQ(j2->identity_data["nis"], "12345");
}

TEST(ProductionHardening, Queue_JobProtoRoundTrip_FullFields){
#ifdef HAS_PROTOBUF
  queue::SubmissionJob j;
  j.job_id="e2e1"; j.exam_id=9; j.student_name="Budi"; j.exam_number="01";
  j.student_class="XII-A"; j.start_time="2026-09-01T08:00:00Z";
  j.mac_address="aa:bb"; j.enqueued_at="2026-09-01T08:05:00Z"; j.retries=1;
  j.answers={{"1","A"},{"2","B"}};
  j.identity_data={{"nis","12345"}};
  auto j2=queue::SubmissionJob::from_protobuf(j.to_protobuf());
  ASSERT_TRUE(j2.has_value()) << "from_protobuf harus bisa parse payload sendiri";
  EXPECT_EQ(j2->job_id, "e2e1");
  EXPECT_EQ(j2->exam_id, 9);
  EXPECT_EQ(j2->student_name, "Budi");
  EXPECT_EQ(j2->exam_number, "01");
  EXPECT_EQ(j2->student_class, "XII-A");
  EXPECT_EQ(j2->mac_address, "aa:bb");
  EXPECT_EQ(j2->start_time, "2026-09-01T08:00:00Z");
  EXPECT_EQ(j2->enqueued_at, "2026-09-01T08:05:00Z");
  EXPECT_EQ(j2->retries, 1);
  ASSERT_EQ(j2->answers.size(), 2u);
  EXPECT_EQ(j2->answers["1"], "A");
  EXPECT_EQ(j2->answers["2"], "B");
  ASSERT_EQ(j2->identity_data.size(), 1u);
  EXPECT_EQ(j2->identity_data["nis"], "12345");
#else
  GTEST_SKIP() << "HAS_PROTOBUF tidak aktif di build ini";
#endif
}

// ----------------------------------------------------------------------
// Sequence sync PG (setval) — setelah restore backup, sequence bisa
// ketinggalan dari MAX(id) sehingga nextval mengembalikan id yang sudah
// dipakai → INSERT gagal (409/PK violation). migrate() harus sync.
// ----------------------------------------------------------------------

TEST(ProductionHardening, Migrate_SyncsSequencesAfterRestore){
  auto c = read_source_file("src/store/exam_store_postgres.cpp");
  EXPECT_NE(c.find("setval"), std::string::npos) << "migrate harus sync sequence (setval)";
  EXPECT_NE(c.find("pg_get_serial_sequence"), std::string::npos);
  EXPECT_NE(c.find("\"exams\""), std::string::npos);
  EXPECT_NE(c.find("\"submissions\""), std::string::npos);
  EXPECT_NE(c.find("\"student_access_logs\""), std::string::npos);
}

// ----------------------------------------------------------------------
// E2E — alur penuh: upload PDF → verifikasi R2 → start → submit →
// worker score (questions_json nyata) → result
// ----------------------------------------------------------------------

TEST(ProductionHardening, E2E_FullFlow_UploadStartSubmitScoreResult){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");

  // 1) Upload PDF (multipart) — create_exam: upload R2 + verifikasi HEAD.
  std::string boundary="----E2E";
  Request cr; cr.body=multipart_pdf(boundary,"soal.pdf","%PDF-1.4 e2e flow\n%%EOF\n");
  cr.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  int id=std::stoi(json_field(created.body,"id"));
  auto after_create=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(after_create.has_value());
  EXPECT_EQ(after_create->file_path, "soal.pdf");

  // 2) Simpan soal — questions_json dipersist & dipakai scorer worker.
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":[{\"number\":1,\"type\":\"single_choice\",\"weight\":1.0,\"key\":\"A\"},"
          "{\"number\":2,\"type\":\"single_choice\",\"weight\":1.0,\"key\":\"B\"}]}";
  ASSERT_EQ(save_exam_questions(sq).status,200);

  // 3) Start ujian.
  examvan::store::active_store()->update(id,[](examvan::models::Exam& e){
    e.status="active"; e.exam_started_at="2026-09-01T00:00:00Z";
  });

  // 4) Submit siswa — hook menangkap job nyata (answers + identitas).
  queue::SubmissionJob captured;
  set_submit_enqueue_hook_for_test([&](const queue::SubmissionJob& j){ captured=j; });
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["Content-Type"]="application/json";
  rq.body="{\"student_name\":\"Budi\",\"exam_number\":\"01\",\"student_class\":\"XII-A\","
          "\"mac_address\":\"aa:bb:cc:dd\",\"answers\":{\"1\":\"A\",\"2\":\"C\"}}";
  auto exam_for_token=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam_for_token.has_value());
  rq.headers["X-Exam-Token"]=exam_for_token->token;
  auto submit_res=submit_exam(rq);
  ASSERT_EQ(submit_res.status,202) << submit_res.body;
  ASSERT_FALSE(captured.job_id.empty());
  set_submit_enqueue_hook_for_test(nullptr);

  // 5) Worker score: queue in-memory + scorer pakai questions_json exam.
  // Worker.start() menjalankan 8 thread worker yang berbagi q_store/results
  // secara konkuren — semua akses HARUS disinkronkan mutex (tanpa ini data
  // race: pop_front() menghancurkan string saat front() menyalinnya).
  std::deque<std::string> q_store;
  std::map<std::string,std::string> results;
  std::mutex q_mu, r_mu;
  queue::SubmissionQueue q(
    [&](const std::string&, const std::string& v){ std::lock_guard<std::mutex> g(q_mu); q_store.push_back(v); },
    [&](const std::string&, int)->std::optional<std::string>{
      std::lock_guard<std::mutex> g(q_mu);
      if(q_store.empty()) return std::nullopt;
      auto v=q_store.front(); q_store.pop_front(); return v;
    },
    [&](const std::string& k, const std::string& v){ std::lock_guard<std::mutex> g(r_mu); results[k]=v; }
  );
  { std::lock_guard<std::mutex> g(q_mu); q_store.push_back(captured.to_json()); }
  auto scorer_fn=[](const queue::SubmissionJob& job)->std::optional<double>{
    auto ex=examvan::store::active_store()->get_by_id(job.exam_id);
    if(!ex) return std::nullopt;
    return examvan::scoring::score_submission_json(ex->questions_json.value_or(""), job.answers);
  };
  queue::Worker w(&q, scorer_fn);
  w.start();
  const std::string result_key=std::string(queue::kResultKeyPrefix)+captured.job_id;
  int waited=0;
  bool found=false;
  while(waited<200){
    {
      std::lock_guard<std::mutex> g(r_mu);
      if(results.find(result_key)!=results.end()){ found=true; break; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25)); waited++;
  }
  w.stop();
  std::lock_guard<std::mutex> g(r_mu);
  auto it=results.find(result_key);
  ASSERT_NE(it, results.end()) << "worker harus menulis result utk job " << captured.job_id;
  EXPECT_NE(it->second.find("\"success\":true"), std::string::npos) << it->second;
  // Jawaban {1:A benar, 2:C salah} dari 2 soal → score 50.0.
  EXPECT_NE(it->second.find("\"score\":50"), std::string::npos) << it->second;
  reset_r2_flags();
}

// ----------------------------------------------------------------------
// Rate limit per exam+MAC (presence bucket Redis) — paritas Go
// ratelimit:presence: (access_log) & ratelimit:submit: (submit_exam)
// ----------------------------------------------------------------------

TEST(ProductionHardening, RateLimitKey_ExamMacVsIpFallback){
  EXPECT_EQ(presence_rate_key("ratelimit:presence:", 7, "aa:bb", "1.2.3.4"),
            "ratelimit:presence:7:aa:bb");
  EXPECT_EQ(presence_rate_key("ratelimit:presence:", 7, "unknown", "1.2.3.4"),
            "ratelimit:presence:7:ip:1.2.3.4");
  EXPECT_EQ(presence_rate_key("ratelimit:submit:", 7, "", "1.2.3.4"),
            "ratelimit:submit:7:ip:1.2.3.4");
  EXPECT_EQ(presence_rate_key("ratelimit:presence:", 7, "aa:bb", ""),
            "ratelimit:presence:7:aa:bb");
}

TEST(ProductionHardening, RateLimit_PresenceAndSubmitGo){
  auto c=read_source_file("src/handlers/api/exams.cpp");
  EXPECT_NE(c.find("ratelimit:presence:"), std::string::npos) << "access_log bucket";
  EXPECT_NE(c.find("ratelimit:submit:"), std::string::npos) << "submit_exam bucket";
  EXPECT_NE(c.find("Terlalu banyak request. Silakan coba lagi nanti."), std::string::npos) << "429 access_log";
  EXPECT_NE(c.find("Terlalu banyak percobaan submit. Silakan coba lagi nanti."), std::string::npos) << "429 submit";
  EXPECT_NE(c.find("INCR"), std::string::npos) << "bucket via INCR";
  EXPECT_NE(c.find("EXPIRE"), std::string::npos) << "TTL window via EXPIRE";
}

// ----------------------------------------------------------------------
// Heartbeat presence (paritas Go) — SET heartbeat:{exam}:{mac} EX 300 +
// LPUSH examvan:heartbeats:pending, lalu HeartbeatFlusher drain → PG
// ----------------------------------------------------------------------

TEST(ProductionHardening, HeartbeatPayload_ParseValid){
  auto hb=queue::parse_heartbeat_payload(
    "{\"exam_id\":7,\"mac_address\":\"aa:bb\",\"student_name\":\"Ani\",\"exam_number\":\"02\","
    "\"student_class\":\"XI-B\",\"device_info\":\"Xiaomi\",\"ip_address\":\"1.2.3.4\","
    "\"event\":\"heartbeat\",\"last_seen\":\"2026-09-01T08:00:00Z\"}");
  ASSERT_TRUE(hb.has_value());
  EXPECT_EQ(hb->exam_id, 7);
  EXPECT_EQ(hb->mac_address, "aa:bb");
  EXPECT_EQ(hb->student_name, "Ani");
  EXPECT_EQ(hb->exam_number, "02");
  EXPECT_EQ(hb->student_class, "XI-B");
  EXPECT_EQ(hb->device_info, "Xiaomi");
  EXPECT_EQ(hb->ip_address, "1.2.3.4");
  EXPECT_EQ(hb->event, "heartbeat");
  EXPECT_EQ(hb->last_seen, "2026-09-01T08:00:00Z");
}

TEST(ProductionHardening, HeartbeatPayload_ParseInvalid){
  EXPECT_FALSE(queue::parse_heartbeat_payload("").has_value());
  EXPECT_FALSE(queue::parse_heartbeat_payload("not-json").has_value());
  EXPECT_FALSE(queue::parse_heartbeat_payload("{\"foo\":1}").has_value());
}

TEST(ProductionHardening, Heartbeat_AccessLogAndFlusherGo){
  auto c=read_source_file("src/handlers/api/exams.cpp");
  EXPECT_NE(c.find("\"heartbeat:\""), std::string::npos) << "SET heartbeat:{exam}:{mac}";
  EXPECT_NE(c.find("EX 300"), std::string::npos) << "TTL 5 menit (Go heartbeatTTL)";
  EXPECT_NE(c.find("examvan:heartbeats:pending"), std::string::npos) << "LPUSH queue";
  auto q=read_source_file("src/queue/submission_queue.cpp");
  EXPECT_NE(q.find("RPOP"), std::string::npos) << "flusher drain via RPOP";
  EXPECT_NE(q.find("kHeartbeatQueueKey"), std::string::npos)
    << "flusher harus pakai konstanta queue yang sama (examvan:heartbeats:pending)";
  EXPECT_NE(q.find("INSERT INTO student_access_logs"), std::string::npos);
  auto m=read_source_file("src/main.cpp");
  EXPECT_NE(m.find("HeartbeatFlusher"), std::string::npos) << "flusher harus di-start di main";
}

// ----------------------------------------------------------------------
// Smoke test menemukan bug produksi nyata: create_exam TIDAK pernah mengisi
// created_by, padahal schema Go punya FK exams_created_by_fkey → admin_users(id).
// INSERT selalu gagal (silent) → response menyesatkan "custom_token already in
// use" (409) padahal akar masalahnya FK violation. Fix: admin_api menginjeksi
// X-Internal-Admin-Id dari session terverifikasi; create_exam memakainya.
// ----------------------------------------------------------------------

TEST(ProductionHardening, CreateExam_SetsCreatedByFromSessionHeader){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request cr; cr.body="name=CreatedByTest&file_path=soal.pdf&size_bytes=100";
  cr.headers["X-Internal-Admin-Id"]="42";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  int id=std::stoi(json_field(created.body,"id"));
  auto saved=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(saved.has_value());
  EXPECT_EQ(saved->created_by,42) << "created_by harus diambil dari session admin";
}

TEST(ProductionHardening, CreateExam_CreatedByDefaultsZeroWithoutHeader){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request cr; cr.body="name=CreatedByDefault&file_path=soal.pdf&size_bytes=100";
  auto created=create_exam(cr);
  ASSERT_EQ(created.status,201) << created.body;
  int id=std::stoi(json_field(created.body,"id"));
  auto saved=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(saved.has_value());
  EXPECT_EQ(saved->created_by,0);
}

TEST(ProductionHardening, AdminApiWrapper_InjectsSessionAdminId){
  auto c=read_source_file("src/http/router_full.cpp");
  EXPECT_NE(c.find("X-Internal-Admin-Id"), std::string::npos)
    << "admin_api harus menginjeksi admin_id session ke handler";
  // Nilai harus berasal dari session terverifikasi, bukan header klien.
  size_t p=c.find("X-Internal-Admin-Id");
  ASSERT_NE(p,std::string::npos);
  EXPECT_NE(c.rfind("admin_id",p), std::string::npos)
    << "header diisi dari SessionData.admin_id (bukan nilai klien)";
  auto e=read_source_file("src/handlers/admin/exams.cpp");
  EXPECT_NE(e.find("X-Internal-Admin-Id"), std::string::npos)
    << "create_exam harus membaca header internal admin id";
  EXPECT_NE(e.find("created_by"), std::string::npos)
    << "create_exam harus mengisi exam.created_by";
}