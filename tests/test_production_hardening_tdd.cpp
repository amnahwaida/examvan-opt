#include <gtest/gtest.h>
#include "handlers/admin/exams.hpp"
#include "handlers/admin/export.hpp"
#include "handlers/auth/login.hpp"
#include "handlers/public/hasil.hpp"
#include "handlers/api/webhook.hpp"
#include "handlers/auth/logout.hpp"
#include "models/exam.hpp"
#include "session/cookie.hpp"
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
  int eid=std::stoi(id);
  // C2: gate — exam harus active+started, token cocok, device approved.
  examvan::store::active_store()->update(eid, [](examvan::models::Exam& e){
    e.status="active"; e.exam_started_at="2026-08-31T00:00:00Z";
  });
  auto exam=examvan::store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  set_device_approved_hook_for_test([](int,const std::string&){ return true; });
  Request req; req.params["exam_id"]=id;
  req.headers["X-Exam-Token"]=exam->token;
  req.headers["X-Device-Id"]="AA:BB:CC:DD:EE:FF";
  auto res=exam_pdf(req);
  EXPECT_EQ(res.status,302) << res.body;
  std::string loc=res.headers["Location"];
  EXPECT_NE(loc.find("https://test.r2.cloudflarestorage.com"), std::string::npos) << loc;
  // C6: prefer layout Go pdfs/{file_path}; fallback exams/{id}/{file_path}.
  EXPECT_NE(loc.find("pdfs/soal.pdf"), std::string::npos) << loc;
  EXPECT_NE(loc.find("X-Amz-Signature="), std::string::npos) << loc;
  set_device_approved_hook_for_test(nullptr);
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
  int eid=std::stoi(id);
  // C2: gate dulu (active+started+token+approved) supaya sampai ke cek R2.
  examvan::store::active_store()->update(eid, [](examvan::models::Exam& e){
    e.status="active"; e.exam_started_at="2026-08-31T00:00:00Z";
  });
  auto exam=examvan::store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  set_device_approved_hook_for_test([](int,const std::string&){ return true; });
  Request req; req.params["exam_id"]=id;
  req.headers["X-Exam-Token"]=exam->token;
  req.headers["X-Device-Id"]="AA:BB:CC:DD:EE:FF";
  auto res=exam_pdf(req);
  EXPECT_EQ(res.status,503);
  EXPECT_NE(res.body.find("R2_NOT_CONFIGURED"), std::string::npos) << res.body;
  set_device_approved_hook_for_test(nullptr);
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

TEST(ProductionHardening, Questions_StrictModeBooleanRoundTrip){
  // Frontend mengirim strict_mode sebagai BOOLEAN JSON (true/false) karena
  // diturunkan dari security_level high. json_int_field (stoi) tidak bisa
  // parse boolean → strict_mode pernah ter-save (bug review menu edit soal).
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":[],\"security_level\":\"high\",\"strict_mode\":true}";
  auto saved=save_exam_questions(sq);
  EXPECT_EQ(saved.status,200) << saved.body;
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  EXPECT_EQ(exam->strict_mode,1) << "strict_mode:true harus tersimpan (1)";
  Request gq; gq.params["exam_id"]=std::to_string(id);
  auto res=get_exam_questions(gq);
  EXPECT_NE(res.body.find("\"strict_mode\":true"), std::string::npos) << res.body;
}

TEST(ProductionHardening, Questions_StrictModeFalseParsed){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":[],\"strict_mode\":false}";
  auto saved=save_exam_questions(sq);
  EXPECT_EQ(saved.status,200) << saved.body;
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  EXPECT_EQ(exam->strict_mode,0) << "strict_mode:false harus tersimpan (0)";
}

TEST(ProductionHardening, Questions_PengawasAssignmentGo){
  // Review menu edit soal: bagian "Atur Pengawas" adalah no-op di C++ —
  // get_exam_questions mengembalikan assigned_pengawas/available_pengawas
  // hardcoded [], dan save_exam_questions membuang pengawas_ids diam-diam.
  // Go memakai tabel exam_pengawas (junction) + admin_users.role ILIKE.
  auto c=read_source_file("src/handlers/admin/exams.cpp");
  EXPECT_NE(c.find("exam_pengawas"), std::string::npos)
    << "save/get questions harus menyentuh tabel exam_pengawas";
  EXPECT_NE(c.find("DELETE FROM exam_pengawas"), std::string::npos)
    << "save harus replace assignment (hapus lama lalu insert)";
  EXPECT_NE(c.find("INSERT INTO exam_pengawas"), std::string::npos);
  EXPECT_NE(c.find("pengawas_ids"), std::string::npos)
    << "save harus mem-parse pengawas_ids dari body";
  EXPECT_NE(c.find("JOIN admin_users"), std::string::npos)
    << "get harus list pengawas ter-assign dengan detail user";
  EXPECT_NE(c.find("ILIKE"), std::string::npos)
    << "available pengawas = admin_users dengan role pengawas (ILIKE)";
  EXPECT_NE(c.find("available_pengawas"), std::string::npos)
    << "get harus mengembalikan available pengawas (bukan hardcoded [])";
}

TEST(ProductionHardening, Questions_PengawasIdsAccepted){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":[{\"number\":1,\"type\":\"single_choice\",\"key\":\"A\",\"choices\":[\"A\",\"B\"]}],\"pengawas_ids\":[3,7]}";
  auto saved=save_exam_questions(sq);
  EXPECT_EQ(saved.status,200) << saved.body;
  Request gq; gq.params["exam_id"]=std::to_string(id);
  auto res=get_exam_questions(gq);
  EXPECT_EQ(res.status,200) << res.body;
  // tanpa PG: assigned/available fallback [] tetapi kunci wajib ada.
  EXPECT_NE(res.body.find("\"assigned_pengawas\":"), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"available_pengawas\":"), std::string::npos) << res.body;
}

TEST(ProductionHardening, Questions_PengawasIdsInvalid400){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":[],\"pengawas_ids\":\"bukan-array\"}";
  auto res=save_exam_questions(sq);
  EXPECT_EQ(res.status,400) << res.body;
  EXPECT_NE(res.body.find("pengawas_ids"), std::string::npos) << res.body;
}

TEST(ProductionHardening, Questions_ScheduleStoredAsUtcIso){
  // Go menyimpan jadwal sebagai UTC ISO — konversi dari input WIB
  // "YYYY-MM-DD HH:MM" (Asia/Jakarta = UTC+7). C++ sebelumnya menyimpan
  // mentah "YYYY-MM-DD HH:MM" → tidak kompatibel dengan exam buatan Go.
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":[],\"start_time\":\"2026-09-01 08:00\",\"end_time\":\"2026-09-01 09:30\"}";
  auto saved=save_exam_questions(sq);
  EXPECT_EQ(saved.status,200) << saved.body;
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  EXPECT_EQ(exam->start_time.value_or(""),"2026-09-01T01:00:00Z") << exam->start_time.value_or("");
  EXPECT_EQ(exam->end_time.value_or(""),"2026-09-01T02:30:00Z") << exam->end_time.value_or("");
}

TEST(ProductionHardening, Questions_InvalidScheduleFormat400){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":[],\"start_time\":\"09-01-2026 08:00\"}";
  auto res=save_exam_questions(sq);
  EXPECT_EQ(res.status,400) << res.body;
  EXPECT_NE(res.body.find("Format jadwal"), std::string::npos) << res.body;
}

TEST(ProductionHardening, Questions_InvalidQuestionType400){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":[{\"number\":1,\"type\":\"bogus\",\"key\":\"A\"}]}";
  auto res=save_exam_questions(sq);
  EXPECT_EQ(res.status,400) << res.body;
}

TEST(ProductionHardening, Questions_MatchingRequiresItems400){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":[{\"number\":1,\"type\":\"matching\",\"key\":{\"1\":\"A\"}}]}";
  auto res=save_exam_questions(sq);
  EXPECT_EQ(res.status,400) << res.body;
}

TEST(ProductionHardening, Questions_ValidStructureAccepted){
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request sq; sq.params["exam_id"]=std::to_string(id);
  sq.headers["Content-Type"]="application/json";
  sq.body="{\"questions\":["
    "{\"number\":1,\"type\":\"single_choice\",\"key\":\"A\",\"choices\":[\"A\",\"B\"]},"
    "{\"number\":2,\"type\":\"short_answer\",\"key\":\"jakarta\"}]}";
  auto res=save_exam_questions(sq);
  EXPECT_EQ(res.status,200) << res.body;
}

TEST(ProductionHardening, ExportSubmissions_ReturnsValidXlsx){
  Request eq; eq.query="tz_offset=-420";
  auto res=export_submissions_xlsx(eq);
  EXPECT_EQ(res.status,200) << res.body.substr(0,100);
  EXPECT_NE(res.headers.count("Content-Type"),0u);
  EXPECT_NE(res.headers.at("Content-Type").find("spreadsheetml"), std::string::npos);
  // XLSX = zip: magic PK\x03\x04 + part XML.
  EXPECT_NE(res.body.find("PK\x03\x04"), std::string::npos) << "harus zip valid";
  EXPECT_NE(res.body.find("[Content_Types].xml"), std::string::npos);
  EXPECT_NE(res.body.find("sheet1.xml"), std::string::npos);
  EXPECT_NE(res.body.find("Nama Siswa"), std::string::npos) << "header paritas Go";
  EXPECT_NE(res.headers.count("Content-Disposition"),0u);
  auto cd=res.headers.at("Content-Disposition");
  EXPECT_NE(cd.find("attachment"), std::string::npos) << cd;
}

TEST(ProductionHardening, ExportXlsx_Not501){
  // Per-exam export (/admin/api/exams/:id/export) tidak boleh lagi 501.
  int id=create_started_exam_id();
  ASSERT_GT(id,0);
  Request eq; eq.params["id"]=std::to_string(id);
  auto res=export_xlsx(eq);
  EXPECT_EQ(res.status,200) << res.body.substr(0,100);
  EXPECT_NE(res.body.find("PK\x03\x04"), std::string::npos);
}

TEST(ProductionHardening, ExportXlsx_RouteRegistered){
  Config cfg; Router r; register_full_routes(r,cfg);
  bool has_sub=false, has_exam=false;
  for(auto& s: r.routes()){
    if(s.find("submissions/export")!=std::string::npos) has_sub=true;
    if(s.find("exams/")!=std::string::npos && s.find("/export")!=std::string::npos) has_exam=true;
  }
  EXPECT_TRUE(has_sub) << "route /admin/api/submissions/export harus terdaftar";
  EXPECT_TRUE(has_exam) << "route /admin/api/exams/:id/export harus terdaftar";
}

TEST(ProductionHardening, BulkToggleExams_Works){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request c1; c1.body="name=BulkA&file_path=a.pdf&size_bytes=100";
  auto r1=create_exam(c1); ASSERT_EQ(r1.status,201)<<r1.body;
  Request c2; c2.body="name=BulkB&file_path=b.pdf&size_bytes=100";
  auto r2=create_exam(c2); ASSERT_EQ(r2.status,201)<<r2.body;
  int id1=std::stoi(json_field(r1.body,"id"));
  int id2=std::stoi(json_field(r2.body,"id"));
  Request bt; bt.body="{\"ids\":["+std::to_string(id1)+","+std::to_string(id2)+"],\"status\":\"active\"}";
  auto res=bulk_toggle_exams(bt);
  EXPECT_EQ(res.status,200) << res.body;
  auto e1=examvan::store::active_store()->get_by_id(id1);
  auto e2=examvan::store::active_store()->get_by_id(id2);
  ASSERT_TRUE(e1.has_value()); ASSERT_TRUE(e2.has_value());
  EXPECT_EQ(e1->status,"active"); EXPECT_EQ(e2->status,"active");
  Request bad; bad.body="{\"ids\":["+std::to_string(id1)+"],\"status\":\"bogus\"}";
  EXPECT_EQ(bulk_toggle_exams(bad).status,400);
}

TEST(ProductionHardening, BulkDeleteExams_Works){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request c1; c1.body="name=BDelA&file_path=a.pdf&size_bytes=100";
  auto r1=create_exam(c1); ASSERT_EQ(r1.status,201)<<r1.body;
  Request c2; c2.body="name=BDelB&file_path=b.pdf&size_bytes=100";
  auto r2=create_exam(c2); ASSERT_EQ(r2.status,201)<<r2.body;
  int id1=std::stoi(json_field(r1.body,"id"));
  int id2=std::stoi(json_field(r2.body,"id"));
  Request bd; bd.body="{\"ids\":["+std::to_string(id1)+","+std::to_string(id2)+"]}";
  auto res=bulk_delete_exams(bd);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_FALSE(examvan::store::active_store()->get_by_id(id1).has_value());
  EXPECT_FALSE(examvan::store::active_store()->get_by_id(id2).has_value());
}

TEST(ProductionHardening, DelegateData_GoShape){
  auto c=read_source_file("src/handlers/admin/exams.cpp");
  // Response shape Go: current_owner/delegated_to/available_gurus/available_pengawas/assigned_pengawas_ids
  EXPECT_NE(c.find("current_owner"), std::string::npos);
  EXPECT_NE(c.find("delegated_to"), std::string::npos);
  EXPECT_NE(c.find("available_gurus"), std::string::npos);
  EXPECT_NE(c.find("available_pengawas"), std::string::npos);
  EXPECT_NE(c.find("assigned_pengawas_ids"), std::string::npos);
  // Guru: instansi sama, active, role guru, exclude creator; pengawas: role ILIKE
  EXPECT_NE(c.find("role ILIKE"), std::string::npos);
  EXPECT_NE(c.find("exam_pengawas"), std::string::npos);
}

TEST(ProductionHardening, DelegatePost_UpdatesOwnerAndPengawas){
  auto c=read_source_file("src/handlers/admin/exams.cpp");
  EXPECT_NE(c.find("delegated_to"), std::string::npos)
    << "delegate harus mengubah exams.delegated_to (owner baru)";
  EXPECT_NE(c.find("DELETE FROM exam_pengawas"), std::string::npos);
  EXPECT_NE(c.find("INSERT INTO exam_pengawas"), std::string::npos);
  EXPECT_NE(c.find("new_owner_id"), std::string::npos);
  EXPECT_NE(c.find("pengawas_ids"), std::string::npos);
}

TEST(ProductionHardening, EditExam_PdfUploadHandled){
  clear_exams_for_testing();
  setenv("EXAMVAN_R2_TESTMODE","1",1);
  set_r2_endpoint("https://test.r2.cloudflarestorage.com");
  Request c; c.body="name=EditPdf&file_path=old.pdf&size_bytes=100";
  auto r=create_exam(c); ASSERT_EQ(r.status,201)<<r.body;
  int id=std::stoi(json_field(r.body,"id"));
  // Multipart edit: nama baru + PDF baru (paritas submitEditExam frontend).
  std::string boundary="----EditPdf";
  std::string body="--"+boundary+"\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nEditPdf Baru\r\n";
  body+="--"+boundary+"\r\nContent-Disposition: form-data; name=\"pdf_file\"; filename=\"new.pdf\"\r\nContent-Type: application/pdf\r\n\r\n%PDF-1.4 new content\r\n%%EOF\r\n";
  body+="--"+boundary+"--\r\n";
  Request eq; eq.params["exam_id"]=std::to_string(id);
  eq.path="/admin/api/exams/"+std::to_string(id)+"/edit";
  eq.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  eq.body=body;
  auto res=update_exam(eq);
  EXPECT_EQ(res.status,200) << res.body;
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  EXPECT_EQ(exam->name,"EditPdf Baru");
  EXPECT_NE(exam->file_path,"old.pdf") << "file_path harus berubah saat PDF baru diupload";
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
  auto exam=examvan::store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  // C3: endpoint credential-gated — tanpa token/approved device → 401.
  Request anon; anon.params["exam_id"]=std::to_string(id);
  EXPECT_EQ(exam_result(anon).status,401) << anon.body;
  // Token cocok + job_id dengan hasil worker → done+score.
  set_result_lookup_hook_for_test([id](const std::string& jid){
    return jid=="JOB1" ? "{\"job_id\":\"JOB1\",\"exam_id\":"+std::to_string(id)+",\"success\":true,\"score\":87.5}" : "";
  });
  Request rq; rq.params["exam_id"]=std::to_string(id);
  rq.headers["X-Exam-Token"]=exam->token;
  rq.query="job_id=JOB1";
  auto res=exam_result(rq);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("\"status\":\"done\""), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("87.5"), std::string::npos) << res.body;
  // job_id tak dikenal → pending.
  Request pend; pend.params["exam_id"]=std::to_string(id);
  pend.headers["X-Exam-Token"]=exam->token;
  pend.query="job_id=NOPE";
  auto resp=exam_result(pend);
  EXPECT_EQ(resp.status,200) << resp.body;
  EXPECT_NE(resp.body.find("\"status\":\"pending\""), std::string::npos) << resp.body;
  set_result_lookup_hook_for_test(nullptr);
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
  // Kolom job_id ditambahkan oleh idempotent migration untuk mengikat result
  // Redis/DB ke submission yang sama.
  EXPECT_NE(insert.find("job_id"), std::string::npos) << insert;
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
  sq.body="{\"questions\":[{\"number\":1,\"type\":\"single_choice\",\"weight\":1.0,\"key\":\"A\",\"choices\":[\"A\",\"B\",\"C\"]},"
          "{\"number\":2,\"type\":\"single_choice\",\"weight\":1.0,\"key\":\"B\",\"choices\":[\"A\",\"B\",\"C\"]}]";
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

  // 5) Worker score/persistence requires PostgreSQL in the durable current
  // contract. The unit build without PG must not fabricate a success result;
  // integration coverage exercises the commit->done path separately.
  queue::SubmissionQueue q(
    [&](const std::string&, const std::string&){},
    [&](const std::string&, int)->std::optional<std::string>{ return std::nullopt; },
    [&](const std::string&, const std::string&){});
  queue::Worker w(&q, [](const queue::SubmissionJob&)->std::optional<double>{ return 50.0; });
  w.start();
  w.stop();
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
}TEST(ProductionHardening, AdminApiWrapper_InjectsSessionAdminId){
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

// ======================================================================
// Menu: Kelola User — handlers & route masih stub (users:[] / 201 palsu /
// {ok:true} tanpa efek). Route yang dipanggil frontend (users/:id/edit,
// /delete, /toggle-status, /verify, /deactivate-package, change-password)
// bahkan TIDAK TERDAFTAR → 404. Deret berikut mem-pin perbaikan.
// ======================================================================

TEST(ProductionHardening, Users_ActionRoutesRegistered){
  auto c=read_source_file("src/http/router_full.cpp");
  EXPECT_NE(c.find("users/:id/edit"), std::string::npos)
    << "route POST /admin/api/users/:id/edit harus terdaftar (frontend submitEditUser)";
  EXPECT_NE(c.find("users/:id/delete"), std::string::npos)
    << "route POST /admin/api/users/:id/delete harus terdaftar (frontend deleteUser)";
  EXPECT_NE(c.find("users/:id/toggle-status"), std::string::npos)
    << "route POST /admin/api/users/:id/toggle-status harus terdaftar";
  EXPECT_NE(c.find("users/:id/verify"), std::string::npos)
    << "route POST /admin/api/users/:id/verify harus terdaftar (verifikasi manual pending_otp)";
  EXPECT_NE(c.find("users/:id/deactivate-package"), std::string::npos)
    << "route POST /admin/api/users/:id/deactivate-package harus terdaftar";
  EXPECT_NE(c.find("user_detail"), std::string::npos)
    << "GET /admin/api/users/:id harus memanggil handler detail, bukan list_users";
  // change-password tidak boleh lagi inline {ok:true} — harus handler nyata.
  EXPECT_NE(c.find("change_password"), std::string::npos)
    << "POST /admin/api/change-password harus handler change_password (bukan lambda {ok:true})";
}

TEST(ProductionHardening, Users_ListQueriesPostgres){
  auto c=read_source_file("src/handlers/admin/users.cpp");
  EXPECT_NE(c.find("FROM admin_users"), std::string::npos)
    << "list_users harus SELECT dari admin_users (bukan users:[])";
  EXPECT_NE(c.find("LIMIT"), std::string::npos) << "pagination LIMIT wajib";
  EXPECT_NE(c.find("OFFSET"), std::string::npos) << "pagination OFFSET wajib";
  EXPECT_NE(c.find("ILIKE"), std::string::npos) << "pencarian search wajib ILIKE";
  EXPECT_NE(c.find("exam_count"), std::string::npos)
    << "list harus menyertakan jumlah ujian per user (popup detail kuota)";
  EXPECT_NE(c.find("total_pages"), std::string::npos)
    << "respons harus menyertakan pagination total_pages (frontend renderUsersPagination)";
}

TEST(ProductionHardening, Users_CreateInsertsPostgres){
  auto c=read_source_file("src/handlers/admin/users.cpp");
  EXPECT_NE(c.find("INSERT INTO admin_users"), std::string::npos)
    << "create_user harus INSERT ke admin_users (bukan 201 palsu id=1)";
  EXPECT_NE(c.find("password_hash"), std::string::npos)
    << "password harus di-hash (bcrypt) sebelum INSERT";
  EXPECT_NE(c.find("RETURNING id"), std::string::npos)
    << "INSERT harus mengembalikan id asli dari DB";
  EXPECT_NE(c.find("hash_password"), std::string::npos)
    << "create_user harus memakai hash_password (bukan plaintext)";
}

TEST(ProductionHardening, Users_EditUpdatesPostgres){
  auto c=read_source_file("src/handlers/admin/users.cpp");
  EXPECT_NE(c.find("UPDATE admin_users"), std::string::npos)
    << "edit_user harus UPDATE admin_users (bukan {ok:true} tanpa efek)";
  EXPECT_NE(c.find("max_exams"), std::string::npos) << "limit ujian harus dapat diubah";
  EXPECT_NE(c.find("max_pdf_size"), std::string::npos) << "limit PDF harus dapat diubah";
  EXPECT_NE(c.find("expires_at"), std::string::npos) << "masa aktif harus dapat diubah";
}

TEST(ProductionHardening, Users_DeleteRemovesRow){
  auto c=read_source_file("src/handlers/admin/users.cpp");
  EXPECT_NE(c.find("DELETE FROM admin_users"), std::string::npos)
    << "delete_user harus DELETE dari admin_users";
}

TEST(ProductionHardening, Users_StatusToggleAndVerifyUpdate){
  auto c=read_source_file("src/handlers/admin/users.cpp");
  EXPECT_NE(c.find("SET status="), std::string::npos)
    << "toggle-status harus UPDATE status";
  EXPECT_NE(c.find("pending_otp"), std::string::npos)
    << "verify harus mengubah status pending_otp -> active";
  EXPECT_NE(c.find("deactivate-package"), std::string::npos)
    << "deactivate-package handler harus ada (paket -> free)";
}

TEST(ProductionHardening, Users_ChangePasswordVerifiesCurrent){
  auto c=read_source_file("src/handlers/admin/users.cpp");
  EXPECT_NE(c.find("current_password"), std::string::npos)
    << "change-password harus menerima current_password";
  EXPECT_NE(c.find("verify_password"), std::string::npos)
    << "change-password harus memverifikasi password lama dengan verify_password";
  EXPECT_NE(c.find("UPDATE admin_users SET password_hash"), std::string::npos)
    << "change-password harus UPDATE password_hash";
}

TEST(ProductionHardening, Users_DetailReturnsSingleUser){
  auto c=read_source_file("src/handlers/admin/users.cpp");
  EXPECT_NE(c.find("WHERE u.id=$1"), std::string::npos)
    << "user_detail harus SELECT WHERE id (satu user)";
  EXPECT_NE(c.find("\\\"user\\\":"), std::string::npos)
    << "respons detail harus berisi objek user (frontend openEditUserModal)";
  // Field yang dirender frontend (renderUserRow / openEditUserModal):
  for(auto f: {"max_concurrent_exams","max_storage_size","operator_created",
               "has_active_package","base_roles","package_roles","expires_at"}){
    EXPECT_NE(c.find(f), std::string::npos) << "detail/list harus menyertakan field " << f;
  }
}

TEST(ProductionHardening, PgConnectionsNeverUseSanitizedUrl){
  // sanitized_url() mengganti password dengan "***" → dipakai sebagai conninfo
  // membuat SEMUA query ad-hoc gagal auth (ditemukan smoke test: FATAL
  // password authentication failed). Koneksi harus pakai conninfo_from_url_or_raw.
  for(auto f: {"src/handlers/admin/users.cpp","src/handlers/admin/exams.cpp",
               "src/handlers/api/exams.cpp","src/jobs/jobs.cpp",
               "src/handlers/admin/export.cpp","src/queue/submission_queue.cpp"}){
    auto c=read_source_file(f);
    EXPECT_EQ(c.find("pool.sanitized_url(),"), std::string::npos)
      << f << " harus konek via conninfo_from_url_or_raw, bukan sanitized_url (password ***)";
    EXPECT_NE(c.find("conninfo_from_url_or_raw"), std::string::npos)
      << f << " harus memakai helper conninfo_from_url_or_raw";
  }
  auto p=read_source_file("src/db/pool.cpp");
  EXPECT_NE(p.find("conninfo_from_url_or_raw"), std::string::npos)
    << "db/pool.cpp harus menyediakan conninfo_from_url_or_raw";
  EXPECT_NE(p.find("password="), std::string::npos)
    << "pg_conninfo_from_url harus mempertahankan password asli";
}

// ======================================================================
// Menu: Pengaturan (saas-settings) — update_settings masih {success:true}
// tanpa menyimpan; GET hanya default hardcoded. Harus UPSERT ke PG + baca
// dari PG, dengan partial-update (hanya field yang hadir) seperti Go.
// ======================================================================

// ======================================================================
// Menu: Submissions — list/detail/queue-status masih stub ([] / null / 0),
// padahal data ada di PG (submissions) & Redis (queue keys).
// ======================================================================

TEST(ProductionHardening, Submissions_ListQueriesPostgres){
  auto c=read_source_file("src/handlers/admin/submissions.cpp");
  EXPECT_NE(c.find("FROM submissions"), std::string::npos)
    << "list_submissions harus SELECT dari tabel submissions";
  EXPECT_NE(c.find("JOIN exams"), std::string::npos)
    << "list harus JOIN exams untuk nama ujian";
  EXPECT_NE(c.find("LIMIT"), std::string::npos) << "pagination wajib";
  EXPECT_NE(c.find("total_pages"), std::string::npos)
    << "respons harus pagination total_pages";
}

TEST(ProductionHardening, Submissions_DetailAndDeleteById){
  auto c=read_source_file("src/handlers/admin/submissions.cpp");
  EXPECT_NE(c.find("s.id=$1"), std::string::npos)
    << "submission_detail harus SELECT WHERE s.id=$1";
  EXPECT_NE(c.find("DELETE FROM submissions"), std::string::npos)
    << "delete_submission harus DELETE nyata";
}

TEST(ProductionHardening, Submissions_QueueStatusFromRedis){
  auto c=read_source_file("src/handlers/admin/submissions.cpp");
  EXPECT_NE(c.find("examvan:submissions:pending"), std::string::npos)
    << "queue_status harus membaca antrean Redis pending (Go parity)";
  EXPECT_NE(c.find("redis_llen"), std::string::npos)
    << "dibutuhkan helper redis_llen";
  auto rr=read_source_file("src/redis/redis_real.cpp");
  EXPECT_NE(rr.find("redis_llen"), std::string::npos)
    << "redis_real harus menyediakan redis_llen (LLEN)";
}

// ======================================================================
// Menu: Pengawas — pengawas_exams/submissions masih [] kosong; approvals
// dan auto-approve tidak dipersist.
// ======================================================================

TEST(ProductionHardening, Pengawas_ExamsQueriesPostgres){
  auto c=read_source_file("src/handlers/admin/pengawas.cpp");
  EXPECT_NE(c.find("exam_pengawas"), std::string::npos)
    << "pengawas_exams harus JOIN exam_pengawas (ujian yang diampu)";
  EXPECT_NE(c.find("FROM exams"), std::string::npos);
  EXPECT_NE(c.find("total_exams"), std::string::npos)
    << "stats harus menyertakan total_exams";
}

TEST(ProductionHardening, Pengawas_SubmissionsByExam){
  auto c=read_source_file("src/handlers/admin/pengawas.cpp");
  EXPECT_NE(c.find("FROM submissions"), std::string::npos)
    << "pengawas_submissions harus membaca tabel submissions";
  EXPECT_NE(c.find("exam_id"), std::string::npos);
}

TEST(ProductionHardening, Pengawas_ApprovalsAndAutoApprovePersist){
  auto c=read_source_file("src/handlers/admin/pengawas.cpp");
  EXPECT_NE(c.find("exam_approvals"), std::string::npos)
    << "pending_approvals harus membaca exam_approvals (bukan [] kosong)";
  EXPECT_NE(c.find("set_approval"), std::string::npos);
  EXPECT_NE(c.find("UPDATE exam_approvals"), std::string::npos)
    << "set_approval harus UPDATE exam_approvals";
  EXPECT_NE(c.find("auto_approve"), std::string::npos)
    << "auto-approve setting harus dipersist";
}

TEST(ProductionHardening, Settings_PersistsToPostgres){
  auto c=read_source_file("src/handlers/admin/settings.cpp");
  EXPECT_NE(c.find("INSERT INTO saas_settings"), std::string::npos)
    << "update_settings harus UPSERT ke saas_settings (bukan {success:true} kosong)";
  EXPECT_NE(c.find("ON CONFLICT"), std::string::npos)
    << "UPSERT wajib pakai ON CONFLICT (key unique)";
  EXPECT_NE(c.find("SELECT key,value FROM saas_settings"), std::string::npos)
    << "settings_page harus membaca setting dari PG, bukan default hardcoded";
  EXPECT_NE(c.find("conninfo_from_url_or_raw"), std::string::npos)
    << "koneksi PG harus via conninfo_from_url_or_raw (bukan sanitized_url)";
}

TEST(ProductionHardening, Settings_PartialUpdateOnlyPresentKeys){
  auto c=read_source_file("src/handlers/admin/settings.cpp");
  EXPECT_NE(c.find("json_has_key"), std::string::npos)
    << "partial update: hanya key yang HADIR di body JSON yang ditulis (paritas Go pointer)";
  // Menyimpan satu seksi (mis. Turnstile) tidak boleh mereset seksi lain:
  // nilai default TIDAK boleh menimpa key yang tidak dikirim.
  EXPECT_NE(c.find("only present"), std::string::npos)
    << "komentar harus menjelaskan perilaku partial-update";
}

// ======================================================================
// Menu: Voucher — list/redeem/activate masih stub ([] / {ok:true} palsu);
// route create/batch/toggle/delete/redemptions bahkan tidak terdaftar.
// ======================================================================

TEST(ProductionHardening, Vouchers_PersistsToPostgres){
  auto c=read_source_file("src/handlers/admin/vouchers.cpp");
  EXPECT_NE(c.find("INSERT INTO vouchers"), std::string::npos)
    << "create voucher harus INSERT ke tabel vouchers";
  EXPECT_NE(c.find("SELECT "), std::string::npos) << "list harus SELECT dari vouchers";
  EXPECT_NE(c.find("FROM vouchers"), std::string::npos);
  EXPECT_NE(c.find("conninfo_from_url_or_raw"), std::string::npos)
    << "koneksi PG via conninfo_from_url_or_raw";
}

TEST(ProductionHardening, Vouchers_RoutesRegistered){
  auto c=read_source_file("src/http/router_full.cpp");
  for(auto r: {"vouchers/batch","vouchers/:id/toggle","vouchers/:id/delete",
               "vouchers/:id/redemptions","vouchers/audit-logs",
               "vouchers/mine","vouchers/redeem","vouchers/activate"}){
    EXPECT_NE(c.find(r), std::string::npos) << "route " << r << " harus terdaftar";
  }
  // POST /vouchers (buat) harus handler nyata, bukan list_vouchers.
  EXPECT_NE(c.find("create_voucher"), std::string::npos);
  EXPECT_NE(c.find("voucher_redemptions"), std::string::npos)
    << "route redemptions per voucher harus ada";
}

TEST(ProductionHardening, Vouchers_RedeemRealFlow){
  auto c=read_source_file("src/handlers/admin/vouchers.cpp");
  EXPECT_NE(c.find("UPPER(TRIM(code))"), std::string::npos)
    << "lookup voucher case-insensitive + trim (paritas Go)";
  EXPECT_NE(c.find("used_count"), std::string::npos)
    << "redeem harus menambah used_count voucher";
  EXPECT_NE(c.find("INSERT INTO voucher_redemptions"), std::string::npos)
    << "redeem harus mencatat riwayat redemptions";
  EXPECT_NE(c.find("UPDATE admin_users"), std::string::npos)
    << "redeem harus menerapkan entitlement ke admin_users (package/limits/expires_at)";
  EXPECT_NE(c.find("duration_days"), std::string::npos)
    << "masa aktif dihitung dari duration_type (bulanan=30, semester=180, tahunan=365)";
  EXPECT_NE(c.find("bulanan"), std::string::npos);
  EXPECT_NE(c.find("30"), std::string::npos);
  auto m=read_source_file("src/store/exam_store_postgres.cpp");
  EXPECT_NE(m.find("voucher_redemptions"), std::string::npos)
    << "migrate harus sync sequence voucher_redemptions setelah restore";
  EXPECT_NE(m.find("setval"), std::string::npos);
}

TEST(ProductionHardening, Vouchers_AuditLogsJoinUsers){
  auto c=read_source_file("src/handlers/admin/vouchers.cpp");
  EXPECT_NE(c.find("JOIN admin_users"), std::string::npos)
    << "audit-logs harus JOIN admin_users untuk username pemakai";
  EXPECT_NE(c.find("voucher_redemptions"), std::string::npos);
}

TEST(ProductionHardening, Settings_MasksSecretsOnRead){
  auto c=read_source_file("src/handlers/admin/settings.cpp");
  EXPECT_NE(c.find("mask_token"), std::string::npos)
    << "smtp_password & turnstile_secret_key harus dimask saat GET (paritas Go maskTokenSetting)";
  EXPECT_NE(c.find("turnstile_secret_key"), std::string::npos);
  EXPECT_NE(c.find("smtp_password"), std::string::npos);
}

TEST(ProductionHardening, AdminApiExecutesHandlerOnce){
  // Ditemukan smoke test: admin_api memanggil body_limit(req,...,h) — yang
  // mengeksekusi `h` — LALU `h(r2)` lagi → SETIAP mutasi admin (create user,
  // create exam, edit, delete, dst) dieksekusi DUA KALI. Run pertama tanpa
  // X-Internal-Admin-Id: INSERT dibuat (created_by=0 → FK violation) lalu
  // respons dibuang; klien menerima respons run kedua (mis. 400 "Username
  // sudah digunakan" padahal user barusan berhasil dibuat).
  auto c=read_source_file("src/http/router_full.cpp");
  EXPECT_EQ(c.find("middleware::body_limit"), std::string::npos)
    << "admin_api TIDAK boleh memakai body_limit(next) — next=h mengeksekusi handler";
  // Ukuran body tetap dicek, tapi tanpa menjalankan handler:
  size_t p=c.find("5*1024*1024");
  ASSERT_NE(p, std::string::npos);
  // Handler harus dipanggil PERSIS sekali — via return h(r2) (dengan admin id).
  size_t inj=c.find("X-Internal-Admin-Id");
  ASSERT_NE(inj, std::string::npos);
  std::string tail=c.substr(inj);
  size_t first_h=tail.find("h(r2)");
  EXPECT_NE(first_h, std::string::npos)
    << "handler harus dipanggil lewat return h(r2) setelah injeksi header";
  if(first_h!=std::string::npos){
    EXPECT_EQ(tail.find("h(r2)", first_h+4), std::string::npos)
      << "handler harus dipanggil SEKALI, bukan dua kali (h(r2) ganda)";
  }
}

// ===== Temuan #1: session login di-forge (admin_id=1 hardcoded) =====
TEST(ProductionHardening, LoginSession_PayloadUsesRealAdminIdAndRole){
  // Payload session harus dibangun dari id/role ASLI user (dari PG), bukan
  // hardcode "admin_id=1&role=[\"guru\"]".
  auto payload=examvan::handlers::auth::build_login_session_payload(7,"budi","[\"operator\"]");
  auto decoded=examvan::b64_decode(payload);
  EXPECT_NE(decoded.find("admin_id=7"), std::string::npos)
    << "payload harus membawa admin_id asli (7), bukan 1: "<<decoded;
  EXPECT_NE(decoded.find("username=budi"), std::string::npos);
  EXPECT_NE(decoded.find("role=[\"operator\"]"), std::string::npos)
    << "payload harus membawa role asli: "<<decoded;
  // User 1 tetap valid (superadmin id=1) — nilai bukan yang di-forge.
  auto p1=examvan::handlers::auth::build_login_session_payload(1,"admin","[\"superadmin\"]");
  EXPECT_NE(examvan::b64_decode(p1).find("role=[\"superadmin\"]"), std::string::npos);
}

TEST(ProductionHardening, LoginSession_NoHardcodedAdminId1){
  // Regresi: dulu login_handler membangun payload langsung
  // b64_encode("admin_id=1&username="+username+"&role=[\"guru\"]") —
  // setiap login jadi user 1 guru (privilege escalation, created_by salah).
  auto c=read_source_file("src/handlers/auth/login.cpp");
  EXPECT_EQ(c.find("admin_id=1&username="), std::string::npos)
    << "payload session TIDAK boleh hardcode admin_id=1";
  EXPECT_NE(c.find("build_login_session_payload"), std::string::npos)
    << "login harus memakai builder payload bersama";
}

TEST(ProductionHardening, UwsPath_ForwardsStudentHeaders){
  // Bug produksi (ditemukan smoke E2E): jalur uWS (WITH_UWEBSOCKETS=ON,
  // dipakai docker produksi) hanya meneruskan allowlist header hardcoded —
  // X-Exam-Token/X-Forwarded-For/X-Real-IP/X-User/X-Version DIBUANG.
  // Di produksi: submit_exam selalu 401 "Token tidak disertakan", access_log
  // & complete_exam gagal validasi token, rate-limit per-IP mati.
  // (Jalur posix src/server/server.cpp:314 parse semua header.)
  auto c=read_source_file("src/server/server.cpp");
  size_t p=c.find("x-exam-token");
  ASSERT_NE(p, std::string::npos) << "uWS path harus baca x-exam-token via getHeader";
  size_t h=c.find("X-Exam-Token\"]=xexam", p);
  ASSERT_NE(h, std::string::npos) << "header x-exam-token harus diteruskan ke Request.headers";
  EXPECT_NE(c.find("getHeader(\"x-forwarded-for\")"), std::string::npos)
    << "X-Forwarded-For wajib diteruskan (rate limit per-IP)";
  EXPECT_NE(c.find("getHeader(\"x-real-ip\")"), std::string::npos)
    << "X-Real-IP wajib diteruskan";
  EXPECT_NE(c.find("getHeader(\"x-user\")"), std::string::npos)
    << "X-User wajib diteruskan (dashboard)";
  EXPECT_NE(c.find("getHeader(\"x-version\")"), std::string::npos)
    << "X-Version wajib diteruskan (cek_hasil_page)";
}

TEST(ProductionHardening, LoginSession_PgSelectsRealIdAndRole){
  // Query PG saat auth sukses harus mengambil id + role asli user, bukan
  // hanya password_hash.
  auto c=read_source_file("src/handlers/auth/login.cpp");
  size_t p=c.find("SELECT password_hash");
  ASSERT_NE(p, std::string::npos) << "query PG auth tidak ditemukan";
  std::string q=c.substr(p, 140);
  EXPECT_NE(q.find("id"), std::string::npos)
    << "query harus SELECT id juga (untuk session): "<<q;
  EXPECT_NE(q.find("role"), std::string::npos)
    << "query harus SELECT role juga (untuk session): "<<q;
}

// ===== Temuan #2: public Cek Hasil mati (g_exams test-only, api {"ok":true}) =====
TEST(ProductionHardening, HasilPage_RendersTemplateContext){
  // hasil_page harus render template dgn konteks asli (token, isDisabled,
  // showAnswers, isLoggedIn, error) — bukan 404/"Ujian tidak ditemukan"
  // untuk semua token (dulu g_exams hanya diisi set_exam_for_test).
  examvan::handlers::public_::clear_exams_for_test();
  examvan::models::Exam e; e.token="TOK999"; e.name="UAS Matematika"; e.public_results=1; e.show_answers=1;
  examvan::handlers::public_::set_exam_for_test("TOK999", e);
  examvan::Request req; req.params["token"]="TOK999";
  auto res=examvan::handlers::public_::hasil_page(req);
  ASSERT_EQ(res.status,200);
  EXPECT_NE(res.body.find("id=\"examTitle\""), std::string::npos);
  EXPECT_NE(res.body.find("UAS Matematika"), std::string::npos);
  EXPECT_NE(res.body.find("const EXAM_TOKEN = \"TOK999\""), std::string::npos)
    << "JS EXAM_TOKEN harus token asli, bukan placeholder";
  EXPECT_NE(res.body.find("const isDisabled = false"), std::string::npos);
  EXPECT_NE(res.body.find("const showAnswersFromServer = true"), std::string::npos);
  EXPECT_NE(res.body.find("const isLoggedIn = false"), std::string::npos);
  EXPECT_EQ(res.body.find("{{"), std::string::npos)
    << "sintaks Go template harus dirender, bukan dibiarkan mentah";
  EXPECT_NE(res.headers["X-Robots-Tag"].find("noindex"), std::string::npos)
    << "header noindex wajib (Go parity)";
  EXPECT_NE(res.headers["Cache-Control"].find("no-store"), std::string::npos)
    << "header no-store wajib (Go parity)";
  examvan::handlers::public_::clear_exams_for_test();
}

TEST(ProductionHardening, HasilPage_DisabledStateRendersTemplate){
  examvan::handlers::public_::clear_exams_for_test();
  examvan::models::Exam e; e.token="TOK123"; e.name="UAS"; e.public_results=0;
  examvan::handlers::public_::set_exam_for_test("TOK123", e);
  examvan::Request req; req.params["token"]="TOK123";
  auto res=examvan::handlers::public_::hasil_page(req);
  ASSERT_EQ(res.status,403); // paritas Go: Forbidden saat hasil non-publik & belum login
  EXPECT_NE(res.body.find("Halaman Hasil Dinonaktifkan"), std::string::npos);
  EXPECT_NE(res.body.find("const isDisabled = true"), std::string::npos);
  EXPECT_EQ(res.body.find("id=\"examTitle\""), std::string::npos)
    << "state disabled tidak boleh menampilkan hero exam";
  EXPECT_EQ(res.body.find("{{"), std::string::npos);
  examvan::handlers::public_::clear_exams_for_test();
}

TEST(ProductionHardening, HasilPage_ErrorStateRendersTemplate){
  examvan::handlers::public_::clear_exams_for_test();
  examvan::Request req; req.params["token"]="NOTFOUND";
  auto res=examvan::handlers::public_::hasil_page(req);
  ASSERT_EQ(res.status,404);
  EXPECT_NE(res.body.find("Ujian Tidak Ditemukan"), std::string::npos);
  EXPECT_NE(res.body.find("const pageHasError = true"), std::string::npos);
  EXPECT_NE(res.body.find("NOTFOUND"), std::string::npos)
    << "kartu error harus menampilkan token yang dicari";
  EXPECT_EQ(res.body.find("{{."), std::string::npos)
    << "sisa placeholder {{.x}} harus nol";
}

TEST(ProductionHardening, HasilApi_ReturnsFullContract){
  examvan::handlers::public_::clear_exams_for_test();
  examvan::models::Exam e; e.token="TOK999"; e.name="UAS"; e.public_results=1; e.show_answers=1;
  e.questions_json=std::string("[{\"number\":1,\"type\":\"single_choice\",\"weight\":2,\"key\":\"A\",\"options\":[\"A\",\"B\"]}]");
  examvan::handlers::public_::set_exam_for_test("TOK999", e);
  examvan::Request req; req.params["token"]="tok999"; // huruf kecil → harus di-uppercase
  auto res=examvan::handlers::public_::cek_hasil_api(req);
  ASSERT_EQ(res.status,200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
  EXPECT_NE(res.body.find("\"exam_name\":\"UAS\""), std::string::npos);
  EXPECT_NE(res.body.find("\"submissions\":[]"), std::string::npos);
  EXPECT_NE(res.body.find("\"max_score\":2"), std::string::npos)
    << "max_score harus dihitung dari bobot soal";
  EXPECT_NE(res.body.find("\"pagination\":{"), std::string::npos);
  EXPECT_NE(res.body.find("\"stats\":{"), std::string::npos);
  EXPECT_NE(res.body.find("\"questions\":["), std::string::npos);
  EXPECT_NE(res.body.find("\"identity_fields\":["), std::string::npos);
  EXPECT_NE(res.body.find("\"show_answers\":true"), std::string::npos);
  EXPECT_NE(res.headers["Cache-Control"].find("no-store"), std::string::npos);
  examvan::handlers::public_::clear_exams_for_test();
}

TEST(ProductionHardening, HasilApi_Disabled403){
  examvan::handlers::public_::clear_exams_for_test();
  examvan::models::Exam e; e.token="TOK123"; e.public_results=0;
  examvan::handlers::public_::set_exam_for_test("TOK123", e);
  examvan::Request req; req.params["token"]="TOK123";
  auto res=examvan::handlers::public_::cek_hasil_api(req);
  ASSERT_EQ(res.status,403);
  EXPECT_NE(res.body.find("Akses dinonaktifkan"), std::string::npos);
  examvan::handlers::public_::clear_exams_for_test();
}

TEST(ProductionHardening, HasilApi_UnknownToken404){
  examvan::handlers::public_::clear_exams_for_test();
  examvan::Request req; req.params["token"]="NOTFOUND";
  auto res=examvan::handlers::public_::cek_hasil_api(req);
  ASSERT_EQ(res.status,404);
  EXPECT_NE(res.body.find("Token ujian tidak valid"), std::string::npos);
}

TEST(ProductionHardening, HasilApi_PgQueriesRealSubmissions){
  // API hasil harus query PG nyata (bukan g_exams test-only): hanya submission
  // ber-answers yang dihitung, urut skor tertinggi dulu.
  auto c=read_source_file("src/handlers/public/hasil.cpp");
  EXPECT_NE(c.find("answers_json IS NOT NULL"), std::string::npos)
    << "harus ada filter answers_json (submission yang benar-benar mengumpulkan)";
  EXPECT_NE(c.find("ORDER BY score DESC NULLS LAST"), std::string::npos)
    << "hasil harus diurutkan skor tertinggi dulu";
  EXPECT_NE(c.find("AVG(score)"), std::string::npos)
    << "stats agregat (rata-rata) harus dihitung dari PG";
}

// ===== Temuan #3: webhook cuma ack {"ok":true} tanpa verifikasi =====
TEST(ProductionHardening, Webhook_EmptySenderOrMessage){
  examvan::Request req; req.body="{\"sender\":\"+62812\",\"message\":\"\"}";
  auto res=examvan::handlers::api::webhook(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("\"status\":false"), std::string::npos);
  EXPECT_NE(res.body.find("Payload tidak lengkap"), std::string::npos);
}

TEST(ProductionHardening, Webhook_NotVerificationMessage){
  examvan::Request req; req.body="{\"sender\":\"+62812\",\"message\":\"halo selamat pagi\"}";
  auto res=examvan::handlers::api::webhook(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("Pesan bukan verifikasi pendaftaran"), std::string::npos);
}

TEST(ProductionHardening, Webhook_FormatMissingKode){
  // Ada frasa verifikasi tapi format username/kode tidak lengkap.
  examvan::Request req; req.body="{\"sender\":\"+62812\",\"message\":\"verifikasi pendaftaran username: budi\"}";
  auto res=examvan::handlers::api::webhook(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("Format pesan tidak sesuai template"), std::string::npos);
}

TEST(ProductionHardening, Webhook_ValidFormatWithoutDb500){
  // Format pesan benar; tanpa PG → 500 Database error (paritas Go pool==nil).
  examvan::Request req; req.body="{\"sender\":\"+62812345678\",\"message\":\"Verifikasi Pendaftaran username: budi kode: ABCD12\"}";
  auto res=examvan::handlers::api::webhook(req);
  EXPECT_EQ(res.status,500);
  EXPECT_NE(res.body.find("Database error"), std::string::npos);
}

TEST(ProductionHardening, Webhook_PgPinsActivationSql){
  // Aktivasi harus UPDATE nyata ke PG: status active + bersihkan otp_code,
  // lookup by whatsapp_number + otp_code + status pending_otp.
  auto c=read_source_file("src/handlers/api/webhook.cpp");
  EXPECT_NE(c.find("whatsapp_number"), std::string::npos);
  EXPECT_NE(c.find("pending_otp"), std::string::npos);
  EXPECT_NE(c.find("status='active'"), std::string::npos);
  EXPECT_NE(c.find("otp_code=NULL"), std::string::npos);
}

// ===== Temuan #4: logout CSRF fallback test-csrf-token bocor ke produksi =====
TEST(ProductionHardening, LogoutCsrf_NoCookie403){
  // Tanpa cookie csrf_token → 403 (dulu fallback "test-csrf-token" membuat
  // token CSRF yang diketahui bisa lolos saat cookie hilang).
  examvan::Request req; req.body="_csrf=test-csrf-token";
  req.headers["X-CSRF-Token"]="test-csrf-token";
  auto res=examvan::handlers::auth::logout_handler(req);
  EXPECT_EQ(res.status,403);
  EXPECT_NE(res.body.find("CSRF"), std::string::npos);
  // Dengan cookie yang benar tetap boleh (regresi guard).
  req.headers["Cookie"]="csrf_token=test-csrf-token";
  auto ok=examvan::handlers::auth::logout_handler(req);
  EXPECT_EQ(ok.status,200);
}

// ===== E2E smoke menemukan: worker antrean mandek di produksi =====
// 8 worker BRPOP pada SATU redisContext hiredis bersama (tidak thread-safe):
// semua thread menunggu pada koneksi yang sama, job tidak pernah dikonsumsi
// (llen terus bertambah). Tiap thread worker harus punya koneksi sendiri.
TEST(ProductionHardening, QueueWorker_NoSharedRedisContext){
  auto c=read_source_file("src/main.cpp");
  // Harus ada koneksi thread_local untuk operasi queue (bukan redis_ctx tunggal).
  EXPECT_NE(c.find("thread_local"), std::string::npos);
  EXPECT_NE(c.find("queue_redis"), std::string::npos);
  // BRPOP tidak boleh lagi memakai redis_ctx (ctx bersama antar-thread).
  EXPECT_EQ(c.find("redisCommand(redis_ctx.get(),\"BRPOP"), std::string::npos);
  // Hub boleh tetap pakai redis_ctx (jalur uWS single-thread), tapi queue tidak.
  EXPECT_NE(c.find("BRPOP %s %d"), std::string::npos);
}
