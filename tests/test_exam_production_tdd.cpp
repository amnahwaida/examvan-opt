#include <gtest/gtest.h>
#include "handlers/admin/exams.hpp"
#include "helpers/utils.hpp"
#include "handlers/r2/r2.hpp"
#include "models/exam.hpp"
#include "utils/log.hpp"
#include <string>
#include <set>
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
  std::string body=multipart_body(boundary, {{"name","Ujian Big"}}, "pdf_file","big.pdf","%PDF-1.4 "+big);
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
  auto body=multipart_body(boundary, {{"name","Ujian R2"}}, "pdf_file","soal.pdf","%PDF-1.4 fake");
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