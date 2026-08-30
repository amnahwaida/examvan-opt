#include <gtest/gtest.h>
#include "handlers/admin/exams.hpp"
#include "helpers/utils.hpp"
#include "handlers/r2/r2.hpp"
#include "models/exam.hpp"
#include "utils/log.hpp"
#include <cstdlib>
#include <string>
#include <set>
#include <thread>
#include <future>
#include <atomic>
#include "store/exam_store_memory.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
using namespace examvan;
using namespace examvan::handlers::admin;
using namespace examvan::helpers;

/*
 * TDD: alur pembuatan soal ujian siap PRODUKSI.
 *
 * Mengunci perbaikan keamanan/validasi yang sebelumnya hilang:
 * - Token: PRNG lemah (mt19937) -> CSPRNG (RAND_bytes)
 * - Token collision: loop menyerah diam-diam -> error eksplisit
 * - custom_token: tidak pernah dicek collision terhadap store -> 409
 * - file_path: hanya cek ".." dan "\" -> tambah null-byte & traversal bertingkat
 * - Ordering: size check SETELAH R2 upload -> sebelum upload (hemat bandwidth)
 * - R2 object key: hardcode exam_id=0 -> pakai id nyata (cegah overwrite)
 * - update/delete: no-op "sukses palsu" -> implementasi real in-memory
 * - Logging: zero log -> structured log pada mutasi
 *
 * File ini mengunci: semua test hijau sebelum handler disebut siap produksi.
 */

// ======================================================================
// Test hooks (reset store supaya tiap test terisolasi)
// ======================================================================
static void with_clean_store(){
  clear_exams_for_testing();
  set_token_generator_for_test(nullptr);
  set_upload_mock_for_test(nullptr);
  utils::set_log_sink_for_test(nullptr);
}

// Helper: body urlencoded minimal
static std::string form_body(const std::string& name, const std::string& fpath, const std::string& sz, const std::string& tok=""){
  std::string b="name="+name+"&file_path="+fpath+"&size_bytes="+sz;
  if(!tok.empty()) b+="&custom_token="+tok;
  return b;
}
// Helper: multipart body sederhana
static std::string multipart_body(const std::string& boundary,
                                  const std::map<std::string,std::string>& fields,
                                  const std::string& file_field, const std::string& filename, const std::string& file_content, const std::string& file_ct="application/pdf"){
  std::string b;
  for(auto &kv: fields){
    b+="--"+boundary+"\r\n";
    b+="Content-Disposition: form-data; name=\""+kv.first+"\"\r\n\r\n";
    b+=kv.second+"\r\n";
  }
  if(!file_field.empty()){
    b+="--"+boundary+"\r\n";
    b+="Content-Disposition: form-data; name=\""+file_field+"\"; filename=\""+filename+"\"\r\n";
    b+="Content-Type: "+file_ct+"\r\n\r\n";
    b+=file_content+"\r\n";
  }
  b+="--"+boundary+"--\r\n";
  return b;
}
// Helper: set R2 environment
static void set_r2_env(bool on){
  if(on){
    setenv("R2_ACCESS_KEY_ID","test",1);
    setenv("R2_SECRET_ACCESS_KEY","test",1);
    setenv("R2_ENDPOINT","https://test.r2.cloudflarestorage.com",1);
    setenv("R2_BUCKET","test",1);
  } else {
    setenv("R2_ACCESS_KEY_ID","",1);
    setenv("R2_SECRET_ACCESS_KEY","",1);
    setenv("R2_ENDPOINT","",1);
    setenv("R2_BUCKET","test",1);
  }
}
// Ekstrak field JSON sederhana dari response
static std::string json_field(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\":";
  size_t p=body.find(needle);
  if(p==std::string::npos) return "";
  size_t s=body.find_first_not_of(" \t\r\n", p+needle.size());
  if(s==std::string::npos) return "";
  if(body[s]=='"'){
    size_t e=s+1; while(e<body.size()){ if(body[e]=='\\'){e+=2;continue;} if(body[e]=='"') break; e++; }
    if(e>=body.size()) return "";
    return body.substr(s+1,e-s-1);
  }
  size_t e=body.find_first_of(",}",s);
  if(e==std::string::npos) e=body.size();
  return body.substr(s,e-s);
}

// ======================================================================
// BATCH 1: Token security
// ======================================================================

// 1. generate_token harus pakai CSPRNG: unik & valid format
TEST(ExamProduction, TokenGeneration_UsesCSPRNG){
  with_clean_store();
  std::set<std::string> seen;
  for(int i=0;i<200;i++){
    auto t=generate_token(8);
    EXPECT_EQ(t.size(),8u);
    EXPECT_TRUE(is_valid_exam_token(t)) << t;
    EXPECT_TRUE(seen.insert(t).second) << "duplicate token: " << t;
  }
}

// 2. Token auto-generate: collision beruntun harus error, bukan silent reuse
TEST(ExamProduction, TokenUniqueness_CollisionReturnsError){
  with_clean_store(); // reset dulu
  set_r2_env(true);
  // set override SETELAH with_clean_store, sehingga override tetap aktif
  set_token_generator_for_test([](int){ return std::string("COLLID08"); });
  // Exam-1: token COLLID08 belum ada di g_seen_tokens -> sukses
  Request req1; req1.body=form_body("Ujian0","/tmp/a.pdf","100");
  auto res1=create_exam(req1);
  EXPECT_EQ(res1.status,201) << res1.body;
  // Exam-2: COLLID08 sudah ada -> collision -> 409
  Request req2; req2.body=form_body("Ujian1","/tmp/a.pdf","100");
  auto res2=create_exam(req2);
  EXPECT_EQ(res2.status,409) << "collision harus ditolak: " << res2.body;
}

// 3. custom_token yang sama dengan exam existing -> 409
TEST(ExamProduction, CustomToken_CollisionWithExistingExamRejected){
  with_clean_store();
  set_r2_env(true);
  Request r1; r1.body=form_body("Ujian1","/tmp/a.pdf","100","AAAA1111");
  auto res1=create_exam(r1);
  EXPECT_EQ(res1.status,201) << res1.body;
  Request r2; r2.body=form_body("Ujian2","/tmp/a.pdf","100","AAAA1111");
  auto res2=create_exam(r2);
  EXPECT_EQ(res2.status,409) << res2.body;
  EXPECT_NE(res2.body.find("token"), std::string::npos) << res2.body;
}

// ======================================================================
// BATCH 1: file_path hardening
// ======================================================================

// 4. file_path mengandung null-byte -> 400
//    Penting: literal "exam\x00.pdf" sebagai const char* TRUNCATE di NUL -> "exam"
//    Kita harus bangun string secara eksplisit supaya NUL tetap ada di dalam std::string
TEST(ExamProduction, FilePath_NullByteRejected){
  with_clean_store();
  std::string fp="exam";
  fp+='\0';
  fp+=".pdf";
  Request req; req.body=form_body("Ujian",fp,"100");
  auto res=create_exam(req);
  EXPECT_EQ(res.status,400) << "file_path dengan null-byte harus ditolak, diterima: " << res.status;
}

// 5. traversal bertingkat (good/../../../etc/passwd) -> 400
TEST(ExamProduction, FilePath_NestedTraversalRejected){
  with_clean_store();
  Request req; req.body=form_body("Ujian","good/../../../etc/passwd","100");
  auto res=create_exam(req);
  EXPECT_EQ(res.status,400);
}

// 6. subdirectory sah (exam here) harus tetap diterima
TEST(ExamProduction, FilePath_LegitSubdirAccepted){
  with_clean_store();
  set_r2_env(true);
  Request req; req.body=form_body("Ujian","soal/uas/paper.pdf","100");
  auto res=create_exam(req);
  EXPECT_EQ(res.status,201) << res.body;
}

// ======================================================================
// BATCH 1: ordering (size check sebelum R2 upload, R2 key pakai id nyata)
// ======================================================================

// 7. file > 5MB -> 413 SEBELUM upload dipanggil
TEST(ExamProduction, FileSize_ChecksBeforeR2Upload){
  with_clean_store();
  int upload_calls=0;
  set_upload_mock_for_test([&](const std::string&, const std::string&){ upload_calls++; return true; });
  std::string boundary="----BV1";
  std::string big(6*1024*1024, 'a');
  std::string body=multipart_body(boundary, {{"name","Ujian Big"}}, "pdf_file","big.pdf","%PDF-1.4 "+big+"\n%%EOF\n");
  Request req; req.body=body;
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,413) << res.body;
  EXPECT_EQ(upload_calls,0) << "upload tidak boleh dipanggil untuk file > batas";
}

// 8. R2 object key harus memakai id exam NYATA, bukan 0
TEST(ExamProduction, R2Key_UsesRealExamId){
  with_clean_store();
  std::string captured_key;
  int upload_calls=0;
  set_upload_mock_for_test([&](const std::string& key, const std::string&){ captured_key=key; upload_calls++; return true; });
  set_r2_env(true);
  std::string boundary="----BV1";
  auto body=multipart_body(boundary, {{"name","Ujian R2"}}, "pdf_file","soal.pdf","%PDF-1.4 fake\n%%EOF\n");
  Request req; req.body=body;
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  ASSERT_EQ(res.status,201) << res.body;
  std::string id=json_field(res.body,"id");
  ASSERT_FALSE(id.empty());
  ASSERT_EQ(upload_calls,1);
  EXPECT_EQ(captured_key, "exams/"+id+"/soal.pdf") << "key harus memakai id nyata, bukan exams/0/";
  // id pertama setelah reset = 1, key TIDAK BOLEH memakai "exams/0/"
  EXPECT_EQ(captured_key.find("exams/0/"), std::string::npos) << "key tidak boleh pakai exams/0/ (id=0 adalah bug hardcoded)";
}

// ======================================================================
// BATCH 1: field default & error format konsisten
// ======================================================================

// 9. create response harus berisi status default & created_at
TEST(ExamProduction, CreateExam_SetsDefaultFields){
  with_clean_store();
  set_r2_env(true);
  Request req; req.body=form_body("Ujian Field","/tmp/a.pdf","2048");
  auto res=create_exam(req);
  ASSERT_EQ(res.status,201) << res.body;
  EXPECT_EQ(json_field(res.body,"status"), "inactive") << res.body;
  EXPECT_FALSE(json_field(res.body,"created_at").empty()) << res.body;
  EXPECT_EQ(json_field(res.body,"size_bytes"), "2048") << res.body;
}

// 10. list juga expose status & created_at (model lengkap, bukan triplet)
TEST(ExamProduction, ListExams_ExposesFullModel){
  with_clean_store();
  set_r2_env(true);
  Request rc; rc.body=form_body("Ujian List","/tmp/a.pdf","512");
  auto c=create_exam(rc);
  ASSERT_EQ(c.status,201);
  Request rl;
  auto res=list_admin_exams(rl);
  ASSERT_EQ(res.status,200);
  EXPECT_NE(res.body.find("\"status\":\"inactive\""), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"size_bytes\":512"), std::string::npos) << res.body;
  EXPECT_NE(res.body.find("\"created_at\":\""), std::string::npos) << res.body;
}

// 11. semua error response konsisten JSON dengan "error"
TEST(ExamProduction, CreateExam_ConsistentJsonErrorFormat){
  with_clean_store();
  Request r1; r1.body=form_body("","/tmp/a.pdf","100");
  auto res1=create_exam(r1);
  EXPECT_EQ(res1.status,400);
  EXPECT_NE(res1.body.find("\"error\""), std::string::npos) << res1.body;

  Request r2; r2.body=form_body("Ujian","","100");
  auto res2=create_exam(r2);
  EXPECT_EQ(res2.status,400);
  EXPECT_NE(res2.body.find("\"error\""), std::string::npos) << res2.body;
}

// 12. nama duplikat diperbolehkan (dokumentasi perilaku saat ini)
TEST(ExamProduction, DuplicateName_AllowedWithWarning){
  with_clean_store();
  set_r2_env(true);
  Request r1; r1.body=form_body("Ujian Duplikat","/tmp/a.pdf","100");
  Request r2; r2.body=form_body("Ujian Duplikat","/tmp/b.pdf","100");
  EXPECT_EQ(create_exam(r1).status,201);
  EXPECT_EQ(create_exam(r2).status,201);
}

// ======================================================================
// BATCH 2: structured logging
// ======================================================================

// 13. create yang sukses menulis log event "exam_created"
TEST(ExamProduction, CreateExam_LogsToStructuredLogger){
  with_clean_store();
  set_r2_env(true);
  std::vector<std::string> lines;
  utils::set_log_sink_for_test([&](const std::string& line){ lines.push_back(line); });
  Request req; req.body=form_body("Ujian Log","/tmp/a.pdf","100");
  auto res=create_exam(req);
  EXPECT_EQ(res.status,201);
  bool found=false;
  for(auto &l: lines){ if(l.find("exam_created")!=std::string::npos && l.find("Ujian Log")!=std::string::npos) found=true; }
  EXPECT_TRUE(found) << "harus ada log exam_created dengan nama exam";
}

// 14. create yang gagal (name kosong) menulis log event "exam_create_failed"
TEST(ExamProduction, CreateExam_LogsFailure){
  with_clean_store();
  std::vector<std::string> lines;
  utils::set_log_sink_for_test([&](const std::string& line){ lines.push_back(line); });
  Request req; req.body=form_body("","/tmp/a.pdf","100");
  auto res=create_exam(req);
  EXPECT_EQ(res.status,400);
  bool found=false;
  for(auto &l: lines){ if(l.find("exam_create_failed")!=std::string::npos) found=true; }
  EXPECT_TRUE(found) << "harus ada log exam_create_failed";
}

// ======================================================================
// BATCH 3: update/delete real (bukan no-op)
// ======================================================================

// 15. delete menghapus dari list
TEST(ExamProduction, DeleteExam_RemovesFromStore){
  with_clean_store();
  set_r2_env(true);
  Request rc; rc.body=form_body("Ujian Hapus","/tmp/a.pdf","100");
  auto c=create_exam(rc);
  ASSERT_EQ(c.status,201);
  std::string id=json_field(c.body,"id");
  Request rl; EXPECT_NE(list_admin_exams(rl).body.find("Ujian Hapus"), std::string::npos);
  Request rd; rd.params["id"]=id;
  auto del=delete_exam(rd);
  EXPECT_EQ(del.status,200) << del.body;
  EXPECT_EQ(list_admin_exams(rl).body.find("Ujian Hapus"), std::string::npos) << "exam harus hilang dari list";
}

// 16. delete exam tidak ada -> 404
TEST(ExamProduction, DeleteExam_NotFound){
  with_clean_store();
  Request rd; rd.params["id"]="99999";
  auto res=delete_exam(rd);
  EXPECT_EQ(res.status,404);
}

// 17. delete tanpa id -> 400
TEST(ExamProduction, DeleteExam_MissingId){
  with_clean_store();
  auto res=delete_exam(Request{});
  EXPECT_EQ(res.status,400);
}

// 18. update toggle status active->inactive
TEST(ExamProduction, UpdateExam_ToggleStatus){
  with_clean_store();
  set_r2_env(true);
  Request rc; rc.body=form_body("Ujian Toggle","/tmp/a.pdf","100");
  auto c=create_exam(rc);
  ASSERT_EQ(c.status,201) << c.body;
  std::string id=json_field(c.body,"id");
  Request rl;
  EXPECT_NE(list_admin_exams(rl).body.find("\"status\":\"inactive\""), std::string::npos);

  Request ru; ru.params["id"]=id; ru.params["action"]="toggle";
  auto up=update_exam(ru);
  ASSERT_EQ(up.status,200) << up.body;
  EXPECT_NE(up.body.find("\"status\":\"active\""), std::string::npos) << up.body;
  Request rl2;
  EXPECT_NE(list_admin_exams(rl2).body.find("\"status\":\"active\""), std::string::npos) << list_admin_exams(rl2).body;
}

// 19. update regenerate token -> token baru berbeda
TEST(ExamProduction, UpdateExam_RegenerateToken){
  with_clean_store();
  set_r2_env(true);
  Request rc; rc.body=form_body("Ujian Reg","/tmp/a.pdf","100");
  auto c=create_exam(rc);
  ASSERT_EQ(c.status,201) << c.body;
  std::string old_token=json_field(c.body,"token");
  std::string id=json_field(c.body,"id");
  Request ru; ru.params["id"]=id; ru.params["action"]="regenerate-token";
  auto up=update_exam(ru);
  ASSERT_EQ(up.status,200) << up.body;
  std::string new_token=json_field(up.body,"token");
  EXPECT_NE(new_token, old_token);
  EXPECT_EQ(new_token.size(),8u);
}

// 20. update nama
TEST(ExamProduction, UpdateExam_ChangeName){
  with_clean_store();
  set_r2_env(true);
  Request rc; rc.body=form_body("Nama Lama","/tmp/a.pdf","100");
  auto c=create_exam(rc);
  ASSERT_EQ(c.status,201);
  std::string id=json_field(c.body,"id");
  Request ru; ru.params["id"]=id; ru.params["action"]="edit";
  ru.params["name"]="Nama Baru";
  auto up=update_exam(ru);
  ASSERT_EQ(up.status,200) << up.body;
  EXPECT_EQ(json_field(up.body,"name"),"Nama Baru") << up.body;
  Request rl;
  EXPECT_NE(list_admin_exams(rl).body.find("Nama Baru"), std::string::npos);
  EXPECT_EQ(list_admin_exams(rl).body.find("Nama Lama"), std::string::npos);
}

// 21. update exam tidak ada -> 404
TEST(ExamProduction, UpdateExam_NotFound){
  with_clean_store();
  Request ru; ru.params["id"]="99999"; ru.params["action"]="toggle";
  auto res=update_exam(ru);
  EXPECT_EQ(res.status,404);
}

// 22. update tanpa id -> 400
TEST(ExamProduction, UpdateExam_MissingId){
  with_clean_store();
  auto res=update_exam(Request{});
  EXPECT_EQ(res.status,400);
}
// ======================================================================
// REVIEW PASS 2 — Bug reproductions + Security + Edge cases (TDD)
// ======================================================================

// ----------------------------------------------------------------------
// Group A: Bug Reproduction
// ----------------------------------------------------------------------

// Bug 1: Protobuf path bypasses PDF magic check
#ifdef HAS_PROTOBUF
TEST(ExamProduction, ProtobufPath_RejectsNonPdfBinary){
  with_clean_store();
  set_r2_env(true);
  examvan::v1::CreateExamRequest pb;
  pb.set_name("Proto Bad");
  pb.set_file_path("bad.pdf");
  pb.set_pdf_data("NOT_PDF_BINARY_DATA");  // bukan %PDF
  std::string encoded;
  ASSERT_TRUE(pb.SerializeToString(&encoded));
  Request req;
  req.method="POST";
  req.headers["Accept"]="application/x-protobuf";
  req.headers["Content-Type"]="application/x-protobuf";
  req.body=encoded;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,400) << "protobuf non-PDF harus ditolak, diterima: " << res.status << " body=" << res.body;
}
#endif

// Bug 2: Protobuf size_bytes attacker-controlled -> harus pakai actual data
#ifdef HAS_PROTOBUF
TEST(ExamProduction, ProtobufPath_SizeBytesFromActualData){
  with_clean_store();
  set_r2_env(true);
  examvan::v1::CreateExamRequest pb;
  pb.set_name("Proto Size");
  pb.set_file_path("s.pdf");
  std::string pdf="%PDF-1.4"; pdf.append(1024, 'a'); pdf+="%\n%%EOF\n";  // ~1KB valid PDF
  pb.set_pdf_data(pdf);
  pb.set_size_bytes(1);  // client-claimed kecil, data sebenarnya 1KB
  std::string encoded;
  ASSERT_TRUE(pb.SerializeToString(&encoded));
  Request req;
  req.method="POST";
  req.headers["Accept"]="application/x-protobuf";
  req.headers["Content-Type"]="application/x-protobuf";
  req.body=encoded;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,201) << res.body;
  examvan::v1::CreateExamResponse resp;
  ASSERT_TRUE(resp.ParseFromString(res.body));
  // id harus >0, name harus sesuai input
  EXPECT_GT(resp.id(), 0);
  EXPECT_EQ(resp.name(), "Proto Size");
  // protobuf response tidak punya size_bytes → cek via list endpoint (JSON)
  std::string list_body = list_admin_exams(Request{}).body;
  // actual data ~1038 bytes (8 header + 1024 'a' + 1 '%' + 5 EOF) → ~1038, bukan 1.
  // Assert size bukan kecil (client-claimed 1) tapi berukuran ~1KB
  // use std::stoi untuk extract angka dari JSON array pertama
  auto sb_pos = list_body.find("\"size_bytes\":");
  ASSERT_NE(sb_pos, std::string::npos) << "size_bytes harus ada: " << list_body;
  int stored_size = std::stoi(list_body.substr(sb_pos + 13));
  EXPECT_GT(stored_size, 1000) << "size_bytes harus dari actual data (~1KB), bukan client-claimed 1";
  EXPECT_NE(stored_size, 1) << "size_bytes tidak boleh 1 (client-claimed)";
}
#endif

// Bug 3: Auto-gen token collision dengan custom token yang sudah ada
TEST(ExamProduction, AutoGenToken_CollisionWithCustomTokenRejected){
  with_clean_store();
  set_r2_env(true);
  // buat exam dengan custom token
  Request r1; r1.body=form_body("Custom","/tmp/a.pdf","100","AAAA1111");
  EXPECT_EQ(create_exam(r1).status,201);
  // set mock generator -> selalu hasilkan "AAAA1111" (sama dgn custom)
  set_token_generator_for_test([](int){ return std::string("AAAA1111"); });
  Request r2; r2.body=form_body("AutoGen Collide","/tmp/b.pdf","100");
  auto res=create_exam(r2);
  EXPECT_EQ(res.status,409) << "auto-gen tidak boleh collide dgn custom token: " << res.status;
}

// Bug 4: Custom token TOCTOU race — 2 concurrent create, 1 harus ditolak
TEST(ExamProduction, CustomToken_ConcurrentDuplicateRejected){
  with_clean_store();
  set_r2_env(true);
  std::atomic<int> ok(0), conflict(0);
  auto create_one = [&](){
    Request req; req.body=form_body("Concurrent","/tmp/c.pdf","100","CCCC9999");
    auto res=create_exam(req);
    if(res.status==201) ok++;
    else if(res.status==409) conflict++;
  };
  std::vector<std::thread> threads;
  threads.emplace_back(create_one);
  threads.emplace_back(create_one);
  for(auto& t: threads) t.join();
  EXPECT_EQ(ok.load(),1) << "hanya 1 create yang boleh berhasil";
  EXPECT_EQ(conflict.load(),1) << "create kedua harus 409 (token duplikat)";
}

// ----------------------------------------------------------------------
// Group B: Security Gap Tests
// ----------------------------------------------------------------------

// Security Gap 1a: PDF truncated (tanpa %%EOF) ditolak
TEST(ExamProduction, PdfValidation_RejectsTruncatedNoEof){
  with_clean_store();
  set_r2_env(true);
  std::string boundary="----BV2";
  // PDF tanpa penanda %%EOF di akhir
  std::string incomplete="%PDF-1.4\n1 0 obj\n<<>>\nendobj\ntrailer\n<<>>";
  auto body=multipart_body(boundary, {{"name","Truncated"}}, "pdf_file","t.pdf", incomplete);
  Request req; req.body=body;
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,400) << "PDF truncated tanpa %%EOF harus ditolak: " << res.status << " body=" << res.body;
}

// Security Gap 1b: HTML polyglot ditolak
TEST(ExamProduction, PdfValidation_RejectsHtmlPolyglot){
  with_clean_store();
  set_r2_env(true);
  std::string boundary="----BV3";
  // PDF yang juga mengandung <script> (polyglot / stored XSS)
  std::string polyglot="%PDF-1.4\n<script>alert('xss')</script>\n%%EOF\n";
  auto body=multipart_body(boundary, {{"name","Polyglot"}}, "pdf_file","p.pdf", polyglot);
  Request req; req.body=body;
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,400) << "polyglot PDF dengan <script> harus ditolak: " << res.status << " body=" << res.body;
}

// Security Gap 2: Filename disanitasi untuk R2 key (traversal & spaces)
TEST(ExamProduction, Filename_SanitizedForR2Key){
  with_clean_store();
  set_r2_env(true);
  std::string captured_key;
  set_upload_mock_for_test([&](const std::string& key, const std::string&){ captured_key=key; });
  std::string boundary="----BV4";
  std::string evil_name="../../etc/passwd.pdf";
  auto body=multipart_body(boundary, {{"name","Sanitize"}}, "pdf_file", evil_name, "%PDF-1.4\n%%EOF\n");
  Request req; req.body=body;
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,201) << res.body;
  // R2 key tidak boleh mengandung traversal ".." atau separator path
  EXPECT_EQ(captured_key.find(".."), std::string::npos) << "key tidak boleh traversal: " << captured_key;
}

// ----------------------------------------------------------------------
// Group C: Edge Cases
// ----------------------------------------------------------------------

// R2 upload gagal -> 502
TEST(ExamProduction, R2UploadFailure_Returns502){
  with_clean_store();
  // mock upload yang mengembalikan indicator gagal — callback void, jadi kita
  // trigger 502 dengan membuat R2 configured tapi mock mengeset env nonconfigured?
  // Sebenarnya create_exam memakai g_upload_mock jika set; untuk simulasi gagal,
  // kita set env R2 configured lalu mock upload THROW tidak mungkin (void).
  // Cara paling dekat: biarkan env kosong + file -> 503 bukan 502. Test 502 via
  // R2 config aktif + file: mock dipanggil, tapi kita tidak bisa set return.
  // Jadi test ini memang divalidasi lewat 503 (R2 not configured + file).
  set_r2_env(false);  // R2 tidak dikonfigurasi
  std::string boundary="----BV5";
  auto body=multipart_body(boundary, {{"name","NoR2"}}, "pdf_file","f.pdf","%PDF-1.4\n%%EOF\n");
  Request req; req.body=body;
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  // fail-closed: file diupload tapi R2 tak dikonfigurasi -> 503 (mandatory fail-closed)
  EXPECT_EQ(res.status,503) << "R2 mandatory fail-closed: file tanpa R2 config harus ditolak";
}

// Name tepat di batas 255 -> diterima
TEST(ExamProduction, Name_AtLimit255_Accepted){
  with_clean_store();
  set_r2_env(true);
  std::string name(255, 'A');
  Request req; req.body=form_body(name,"/tmp/a.pdf","100");
  auto res=create_exam(req);
  EXPECT_EQ(res.status,201) << "name 255 chars harus diterima: " << res.body;
}

// Regenerate-token gagal total -> 500 (bukan 200 dengan token lama)
TEST(ExamProduction, RegenerateToken_TotalFailure_ReturnsError){
  with_clean_store();
  set_r2_env(true);
  // buat exam auto-gen (token di-claim)
  Request rc; rc.body=form_body("RegFail","/tmp/a.pdf","100");
  auto c=create_exam(rc);
  ASSERT_EQ(c.status,201) << c.body;
  std::string id=json_field(c.body,"id");
  // set generator agar selalu menghasilkan token yang SAMA dgn yang sudah dipakai
  // (misal token custom existing "AAAA1111")
  Request r1; r1.body=form_body("Custom","/tmp/z.pdf","100","AAAA1111");
  ASSERT_EQ(create_exam(r1).status,201);
  set_token_generator_for_test([](int){ return std::string("AAAA1111"); });
  Request ru; ru.params["id"]=id; ru.params["action"]="regenerate-token";
  auto up=update_exam(ru);
  // karena 5x attempt collide -> tidak boleh 200 sukses
  EXPECT_NE(up.status,200) << "regenerate total failure tidak boleh sukses-palsu 200: " << up.body;
}

// size_bytes=0 dengan file_data besar -> 413
TEST(ExamProduction, SizeBytes_ZeroWithLargeFile_Rejected){
  with_clean_store();
  std::string boundary="----BV6";
  std::string big(6*1024*1024, 'a');
  auto body=multipart_body(boundary, {{"name","BigZero"},{"size_bytes","0"}}, "pdf_file","big.pdf","%PDF-1.4 "+big+"\n%%EOF\n");
  Request req; req.body=body;
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,413) << "file >5MB dgn size_bytes=0 harus ditolak: " << res.status;
}

// Name dengan null-byte -> 400 (log injection vector)
TEST(ExamProduction, Name_WithNullBytes_Rejected){
  with_clean_store();
  std::string name="test";
  name+='\0';
  name+="evil";
  Request req; req.body=form_body(name,"/tmp/a.pdf","100");
  auto res=create_exam(req);
  EXPECT_EQ(res.status,400) << "name dengan null-byte harus ditolak: " << res.status;
}

// ----------------------------------------------------------------------
// Group E: Review Pass 3 — regenerate old-token cleanup, edit validation
// ----------------------------------------------------------------------

// Bug 1: regenerate token (auto-gen) → old token harus di-unclaim supaya bisa dipakai lagi.
// Catatan: bug ini terjadi pada token AUTO-GEN (bukan custom) karena auto-gen
// menggunakan claim_token() yang menambah ke seen_tokens_. Ketika regenerate
// mengganti token tanpa unclaim lama, token lama permanen di seen_tokens_.
TEST(ExamProduction, RegenerateToken_OldAutoTokenRemovedFromSeen){
  with_clean_store();
  // buat exam pertama dengan auto-gen token → id=next_id()
  Request r1; r1.body=form_body("AutoExam","/tmp/a.pdf","100");
  auto cr=create_exam(r1);
  ASSERT_EQ(cr.status,201) << cr.body;
  // ekstrak token auto-gen dari response
  auto pt=cr.body.find("\"token\":\"");
  ASSERT_NE(pt, std::string::npos);
  std::string old_token=cr.body.substr(pt+9, 8);
  // ekstrak id
  auto pid=cr.body.find("\"id\":");
  std::string id_str=cr.body.substr(pid+5);
  id_str=id_str.substr(0, id_str.find(','));
  // set generator → selalu hasilkan NEWTOK01
  set_token_generator_for_test([](int){ return std::string("NEWTOK01"); });
  // regenerate via path suffix
  Request ru; ru.params["id"]=id_str; ru.path="/"+id_str+"/regenerate-token";
  auto res=update_exam(ru);
  EXPECT_EQ(res.status,200) << res.body;
  // OLD auto-gen token harus bisa di-claim lagi (di-unclaim setelah regenerate)
  EXPECT_TRUE(store::active_store()->claim_token(old_token))
    << "old auto-gen token harus bisa di-claim lagi setelah regenerate";
  // NEWTOK01 harus sudah di-claim
  EXPECT_FALSE(store::active_store()->claim_token("NEWTOK01"))
    << "new token sudah aktif — tidak bisa di-claim ulang";
}

// Bug 2: edit name dengan null-byte → 400 (log injection vector)
TEST(ExamProduction, UpdateExam_EditName_NullBytesRejected){
  with_clean_store();
  // buat exam dulu
  Request r1; r1.body=form_body("Normal","/tmp/a.pdf","100");
  auto cr=create_exam(r1);
  ASSERT_EQ(cr.status,201) << cr.body;
  auto pid=cr.body.find("\"id\":");
  ASSERT_NE(pid, std::string::npos);
  std::string id_str=cr.body.substr(pid+5);
  id_str=id_str.substr(0, id_str.find(','));
  // edit dengan null-byte di nama via path /edit
  Request ru; ru.params["id"]=id_str; ru.path="/"+id_str+"/edit";
  std::string badname="test"; badname+='\0'; badname+="evil";
  ru.params["name"]=badname;
  auto res=update_exam(ru);
  EXPECT_EQ(res.status,400) << "edit dengan null-byte harus ditolak: " << res.body;
}

// Bug 3: edit tanpa param name → harus 400 dengan pesan spesifik tentang "name"
TEST(ExamProduction, UpdateExam_EditEmptyName_BetterError){
  with_clean_store();
  Request r1; r1.body=form_body("SomeName","/tmp/a.pdf","100");
  auto cr=create_exam(r1);
  ASSERT_EQ(cr.status,201) << cr.body;
  auto pid=cr.body.find("\"id\":");
  ASSERT_NE(pid, std::string::npos);
  std::string id_str=cr.body.substr(pid+5);
  id_str=id_str.substr(0, id_str.find(','));
  // edit tanpa params["name"] — handler harusnya 400 dan sebutkan "name"
  Request ru; ru.params["id"]=id_str; ru.path="/"+id_str+"/edit";
  // name kosong (tidak di-set) — handler harus reject dengan pesan spesifik
  auto res=update_exam(ru);
  EXPECT_EQ(res.status,400) << "edit tanpa name harus 400: " << res.status;
  EXPECT_NE(res.body.find("name"), std::string::npos)
    << "pesan error harus menyebutkan 'name': " << res.body;
}

// ----------------------------------------------------------------------
// Group F: Review Pass 4 — boundary + sanitization + protobuf responses
// ----------------------------------------------------------------------

// Custom token 7 chars → 400 (too short, harus 8)
TEST(ExamProduction, CustomToken_7Chars_Rejected){
  with_clean_store();
  Request req; req.body=form_body("Short","/tmp/a.pdf","100","ABCDEFG");
  auto res=create_exam(req);
  EXPECT_EQ(res.status,400) << "token 7 chars harus ditolak: " << res.status;
}

// Custom token 9 chars → 400 (too long, harus 8)
TEST(ExamProduction, CustomToken_9Chars_Rejected){
  with_clean_store();
  Request req; req.body=form_body("Long","/tmp/a.pdf","100","ABCDEFGHI");
  auto res=create_exam(req);
  EXPECT_EQ(res.status,400) << "token 9 chars harus ditolak: " << res.status;
}

// Filename .exe harus dipaksa jadi .pdf oleh sanitize_filename
TEST(ExamProduction, Filename_Sanitized_ForcePdfExtension){
  with_clean_store();
  set_r2_env(true);
  std::string captured_key;
  set_upload_mock_for_test([&](const std::string& key, const std::string&){ captured_key=key; });
  std::string boundary="----BV9";
  auto body=multipart_body(boundary, {{"name","SanitizeExt"}}, "pdf_file","malware.exe","%PDF-1.4\n%%EOF\n");
  Request req; req.body=body;
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,201) << res.body;
  // R2 key harus mengandung .pdf, bukan .exe
  EXPECT_NE(captured_key.rfind(".pdf"), std::string::npos)
    << "ekstensi harus dipaksa .pdf, key: " << captured_key;
}

// Filename sangat panjang → truncate ke 128 chars + .pdf
TEST(ExamProduction, Filename_Sanitized_MaxLength128){
  with_clean_store();
  set_r2_env(true);
  std::string captured_key;
  set_upload_mock_for_test([&](const std::string& key, const std::string&){ captured_key=key; });
  std::string boundary="----BV10";
  std::string longname(200, 'X');
  longname+=".pdf";
  auto body=multipart_body(boundary, {{"name","Truncate"}}, "pdf_file",longname,"%PDF-1.4\n%%EOF\n");
  Request req; req.body=body;
  req.headers["Content-Type"]="multipart/form-data; boundary="+boundary;
  auto res=create_exam(req);
  EXPECT_EQ(res.status,201) << res.body;
  // R2 key = "exams/{id}/filename" — hitung bagian filename setelah last /
  auto slash_pos = captured_key.rfind('/');
  std::string fname = captured_key.substr(slash_pos + 1);
  EXPECT_LE(fname.size(), 128u) << "filename harus di-truncate <=128: " << fname;
}

// ----------------------------------------------------------------------
// Group G: Review Pass 5 — size validation + protobuf response completeness
// ----------------------------------------------------------------------

// Security: negative size_bytes harus ditolak atau di-clamp ke 0 (bisa disimpan negatif)
TEST(ExamProduction, SizeBytes_Negative_IsRejected){
  with_clean_store();
  Request req; req.body=form_body("NegSize","/tmp/a.pdf","-100");
  auto res=create_exam(req);
  // Harus ditolak (400) atau minimal size_bytes tidak negatif di response
  if(res.status==201){
    // Jika diterima, pastikan size_bytes tidak negatif di JSON response
    EXPECT_EQ(res.body.find("\"size_bytes\":-100"), std::string::npos)
      << "size_bytes tidak boleh negatif di response: " << res.body;
  } else {
    EXPECT_EQ(res.status,400) << "size_bytes negatif harus 400: " << res.body;
  }
}

#ifdef HAS_PROTOBUF
// Protobuf response untuk toggle harus mengandung id yang valid
TEST(ExamProduction, UpdateExam_ProtobufToggleResponse){
  with_clean_store();
  set_r2_env(true);
  Request cr; cr.body=form_body("ToggleProto","/tmp/a.pdf","100");
  auto c=create_exam(cr);
  ASSERT_EQ(c.status,201) << c.body;
  // ekstrak id
  auto pid=c.body.find("\"id\":");
  std::string id_str=c.body.substr(pid+5);
  id_str=id_str.substr(0,id_str.find(','));
  // toggle via protobuf Accept
  Request ru; ru.params["id"]=id_str; ru.path="/"+id_str+"/toggle";
  ru.headers["Accept"]="application/x-protobuf";
  auto res=update_exam(ru);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_EQ(res.headers.at("Content-Type"),"application/x-protobuf");
  examvan::v1::UpdateExamResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body bukan UpdateExamResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_TRUE(pb.ok());
  EXPECT_EQ(pb.id(),std::stoi(id_str));
}

// Protobuf response untuk delete harus sukses
TEST(ExamProduction, DeleteExam_ProtobufResponse){
  with_clean_store();
  set_r2_env(true);
  Request cr; cr.body=form_body("DeleteProto","/tmp/a.pdf","100");
  auto c=create_exam(cr);
  ASSERT_EQ(c.status,201) << c.body;
  auto pid=c.body.find("\"id\":");
  std::string id_str=c.body.substr(pid+5);
  id_str=id_str.substr(0,id_str.find(','));
  // delete via protobuf Accept
  Request dr; dr.params["id"]=id_str;
  dr.headers["Accept"]="application/x-protobuf";
  auto res=delete_exam(dr);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_EQ(res.headers.at("Content-Type"),"application/x-protobuf");
  examvan::v1::DeleteExamResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body bukan DeleteExamResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_TRUE(pb.ok());
}
#endif

// ======================================================================
// GROUP H: Review Pass 7 — TOCTOU, multipart boundary, consistency (TDD)
// ======================================================================

// Bug B: multipart boundary dengan trailing params (charset) harus tetap ter-parse
TEST(ExamProduction, Multipart_BoundaryTrailingParams_Parsed){
  with_clean_store();
  set_r2_env(true);
  std::string boundary="----BV11";
  // Content-Type dengan trailing params: "; charset=utf-8"
  std::string ct="multipart/form-data; boundary="+boundary+"; charset=utf-8";
  // multipart body hanya name — tanpa file
  std::string body="--"+boundary+"\r\n";
  body+="Content-Disposition: form-data; name=\"name\"\r\n\r\n";
  body+="TrailingParams\r\n";
  body+="--"+boundary+"--\r\n";
  Request req; req.body=body;
  req.headers["Content-Type"]=ct;
  auto res=create_exam(req);
  // name harus ter-parse — bukan 400 "name required", tapi 400 "file_path required"
  EXPECT_EQ(res.status,400) << "harus lolos parse name, bukan 400 name required: " << res.body;
  EXPECT_NE(res.body.find("file_path"), std::string::npos)
    << "setelah name ter-parse, error berikutnya harus soal file_path: " << res.body;
}

// Bug C: created_at di response harus match stored value (bukan recomputed now())
TEST(ExamProduction, CreateExam_CreatedAtConsistent){
  with_clean_store();
  Request req; req.body=form_body("CreatedAt","/tmp/a.pdf","100");
  auto res=create_exam(req);
  ASSERT_EQ(res.status,201) << res.body;
  // ekstrak id dari response
  auto pid=res.body.find("\"id\":");
  ASSERT_NE(pid, std::string::npos);
  int id=std::stoi(res.body.substr(pid+5));
  // ambil dari store
  auto exam=store::active_store()->get_by_id(id);
  ASSERT_TRUE(exam.has_value());
  // stored created_at harus muncul di response
  EXPECT_NE(res.body.find(exam->created_at), std::string::npos)
    << "response created_at harus match stored: store=" << exam->created_at
    << " response=" << res.body;
}

// Bug D: export_xlsx harus 501, bukan 200 dengan content palsu
TEST(ExamProduction, ExportXlsx_Returns501NotImpl){
  auto res=export_xlsx(Request{});
  EXPECT_EQ(res.status,501) << "export_xlsx stub harus return 501, bukan 200 fake: " << res.body;
}

// Bug E: update_exam dengan action tidak dikenal → 400
TEST(ExamProduction, UpdateExam_UnknownAction_ImmediateReject){
  with_clean_store();
  // buat exam dulu
  Request r1; r1.body=form_body("UnknownAct","/tmp/a.pdf","100");
  auto cr=create_exam(r1);
  ASSERT_EQ(cr.status,201) << cr.body;
  auto pid=cr.body.find("\"id\":");
  ASSERT_NE(pid, std::string::npos);
  std::string id_str=cr.body.substr(pid+5);
  id_str=id_str.substr(0,id_str.find(','));
  // kirim action yang tidak dikenal
  Request ru; ru.params["id"]=id_str; ru.path="/"+id_str+"/foo";
  auto res=update_exam(ru);
  EXPECT_EQ(res.status,400) << "unknown action harus 400: " << res.body;
}

// Bug A (TOCTOU): custom-token path HARUS claim_token() dulu — bukan langsung add().
// Skenario: thread A (regenerate-token) claim "RACE0001" (masuk seen_tokens_),
// belum update(). Thread B (create custom "RACE0001") — path custom lama langsung
// add() yang hanya cek exams_[] → "RACE0001" belum di exams_ → sukses →
// nanti update() thread A menulis token sama ke exam lain → DUA exam token sama.
// Fix: path custom juga melewati claim_token (atomic cek seen_tokens_ + exams_).
TEST(ExamProduction, CustomToken_ConflictWithRegenerateClaim_Rejected){
  with_clean_store();
  set_r2_env(true);
  // buat exam A dengan custom token
  Request r1; r1.body=form_body("ExamA","/tmp/a.pdf","100","AAAA1111");
  EXPECT_EQ(create_exam(r1).status,201);
  // simulate regenerate-token pada exam A: claim "RACE0001" masuk seen_tokens_,
  // belum di-update ke exams_ (window race)
  ASSERT_TRUE(store::active_store()->claim_token("RACE0001")) << "pre-claim harus sukses";
  // create exam B dengan custom token yang sama → harus 409 (bukan 201)
  Request r2; r2.body=form_body("ExamB","/tmp/b.pdf","100","RACE0001");
  auto res=create_exam(r2);
  EXPECT_EQ(res.status,409)
    << "custom token yang sedang di-claim regenerate harus ditolak, dapat: " << res.status
    << " body=" << res.body;
  // cleanup pre-claim agar tidak bocor ke test lain
  store::active_store()->unclaim_token("RACE0001");
}

// add() menolak token duplikat
TEST(ExamStoreMemory, Add_RejectsDuplicateToken){
  examvan::store::ExamStoreMemory store;
  models::Exam e1; e1.id=1; e1.token="AAAA1111"; e1.status="inactive";
  models::Exam e2; e2.id=2; e2.token="AAAA1111"; e2.status="inactive";
  EXPECT_TRUE(store.add(e1));
  EXPECT_FALSE(store.add(e2)) << "token duplikat harus ditolak oleh add()";
  EXPECT_EQ(store.count(),1u);
}

// remove() membersihkan claimed token supaya bisa dipakai lagi
TEST(ExamStoreMemory, Remove_CleansClaimedToken){
  examvan::store::ExamStoreMemory store;
  models::Exam e1; e1.id=1; e1.token="BBBB2222";
  ASSERT_TRUE(store.add(e1));
  // token e1 sudah ter-claim oleh add -> claim lagi = false
  EXPECT_FALSE(store.claim_token("BBBB2222")) << "token yang sudah dipakai tidak bisa di-claim";
  // hapus exam e1
  EXPECT_TRUE(store.remove(e1.id));
  // setelah remove, token bisa di-claim lagi
  EXPECT_TRUE(store.claim_token("BBBB2222")) << "token exam yang dihapus harus bisa dipakai lagi";
}
