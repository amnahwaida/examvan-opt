#include <gtest/gtest.h>
#include "handlers/admin/exams.hpp"
#include "handlers/admin/users.hpp"
#include "handlers/admin/vouchers.hpp"
#include "handlers/admin/pengawas.hpp"
#include "handlers/admin/submissions.hpp"
#include "handlers/admin/dashboard.hpp"
#include "handlers/admin/settings.hpp"
#include "handlers/api/exams.hpp"
#include "handlers/api/webhook.hpp"
#include "handlers/public/hasil.hpp"
#include "middleware/protobuf.hpp"
#include "config/config.hpp"
#include "examvan.pb.h"
#include "store/exam_store.hpp"
#include "store/exam_store_memory.hpp"
#include <cstdlib>
using namespace examvan;

static std::string prepare_started_exam_for_api(){
  handlers::admin::clear_exams_for_testing();
  setenv("R2_ACCESS_KEY_ID","test",1);
  setenv("R2_SECRET_ACCESS_KEY","test",1);
  setenv("R2_ENDPOINT","https://test.r2.cloudflarestorage.com",1);
  setenv("R2_BUCKET","test",1);
  setenv("EXAMVAN_R2_TESTMODE","1",1); // test harness tanpa R2 nyata: opt-in eksplisit
  Request cr; cr.body="name=ProtoActive&file_path=/tmp/a.pdf&size_bytes=100";
  auto created=handlers::admin::create_exam(cr);
  EXPECT_EQ(created.status,201) << created.body;
  if(created.status!=201) return "";
  size_t p=created.body.find("\"token\":\"");
  if(p==std::string::npos) return "";
  p+=9; size_t e=created.body.find('"',p);
  std::string token=created.body.substr(p,e-p);
  size_t ip=created.body.find("\"id\":");
  if(ip==std::string::npos) return token;
  ip+=5; size_t ie=created.body.find_first_of(",}",ip);
  int id=std::stoi(created.body.substr(ip,ie-ip));
  store::active_store()->update(id,[](models::Exam& ex){ ex.status="active"; ex.exam_started_at="2026-08-31T00:00:00Z"; });
  return token;
}

// Kembalikan id exam yang valid + sudah dimulai (untuk endpoint alur siswa).
static int prepare_started_exam_id_for_api(){
  prepare_started_exam_for_api();
  auto exams=store::active_store()->list_all();
  if(exams.empty()) return -1;
  return exams.back().id;
}

/*
 * TDD Protobuf Handler Migration — 2c/8GB optimal
 *
 * Setiap handler yang mengembalikan data harus bisa melayani respons Protobuf
 * ketika client mengirim Accept: application/x-protobuf. Test ini memastikan:
 * 1. Status code benar (200)
 * 2. Content-Type = application/x-protobuf
 * 3. Body bisa di-decode ke tipe Protobuf yang sesuai
 * 4. Field success terisi
 *
 * Untuk POST/PUT/DELETE (mutation), test memastikan endpoint tetap menerima
 * request JSON tanpa error (no regression). Protobuf decode diuji terpisah
 * untuk mutation yang sudah menerima protobuf request body.
 *
 * Latar: JSON 30-50% lebih boros di hot path (500k msg/menit). Protobuf
 * mandatory mengurangi 40% byte, 60% CPU, 35% RSS sesuai
 * docs/ARCHITECTURE_CPP_UWS_PROTOBUF.md §2.
 */

// ======================================================================
// Helper: build request with Accept: application/x-protobuf
// ======================================================================
static Request pb_accept(const std::string& method = "GET") {
  Request r;
  r.method = method;
  r.headers["Accept"] = "application/x-protobuf";
  return r;
}

static Request pb_content(const std::string& method, const std::string& body) {
  Request r;
  r.method = method;
  r.headers["Accept"] = "application/x-protobuf";
  r.headers["Content-Type"] = "application/x-protobuf";
  r.body = body;
  return r;
}

// ======================================================================
// Admin: Dashboard
// ======================================================================

TEST(ProtobufHandlers, DashboardStats_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::admin::dashboard_stats(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.count("Content-Type") ? res.headers.at("Content-Type") : "", "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::DashboardStats pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid protobuf";
  EXPECT_TRUE(pb.success());
}

TEST(ProtobufHandlers, DashboardStats_JsonStillWorks) {
  Request req;
  req.method = "GET";
  req.headers["Accept"] = "application/json";
  auto res = handlers::admin::dashboard_stats(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

// ======================================================================
// Admin: Users
// ======================================================================

TEST(ProtobufHandlers, ListUsers_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::admin::list_users(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::UserList pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid UserList protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_GE(pb.total(), 0);
}

TEST(ProtobufHandlers, ListUsers_JsonStillWorks) {
  Request req; req.method = "GET";
  auto res = handlers::admin::list_users(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, CreateUser_JsonMutationStillWorks) {
  Request req;
  req.method = "POST";
  req.body = "username=testmig&password=pass12345&role=guru";
  auto res = handlers::admin::create_user(req);
  EXPECT_EQ(res.status, 201);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, CreateUser_EditUser_DeleteUser_StubStillWorks) {
  // handlers bukan stub lagi: butuh id (path /users/:id/...).
  Request e; e.path="/admin/api/users/1/edit"; e.params["id"]="1";
  auto res_edit = handlers::admin::edit_user(e);
  EXPECT_EQ(res_edit.status, 200);
  Request d; d.path="/admin/api/users/1/delete"; d.params["id"]="1";
  auto res_del = handlers::admin::delete_user(d);
  EXPECT_EQ(res_del.status, 200);
}

// ======================================================================
// Admin: Exams
// ======================================================================

TEST(ProtobufHandlers, ListAdminExams_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::admin::list_admin_exams(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::AdminExamList pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid AdminExamList protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_GE(pb.total(), 0);
}

TEST(ProtobufHandlers, ListAdminExams_JsonStillWorks) {
  Request req; req.method = "GET";
  auto res = handlers::admin::list_admin_exams(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, CreateExam_ProtobufInboundDecode) {
  examvan::handlers::admin::clear_exams_for_testing(); // isolasi — token unik per suite
  examvan::v1::CreateExamRequest req_pb;
  req_pb.set_name("Ujian TDD Proto");
  req_pb.set_file_path("/tmp/tdd.pdf");
  req_pb.set_size_bytes(1024);
  req_pb.set_custom_token("PBTEST01");
  std::string encoded;
  ASSERT_TRUE(req_pb.SerializeToString(&encoded));
  auto req = pb_content("POST", encoded);
  auto res = handlers::admin::create_exam(req);
  EXPECT_EQ(res.status, 201);
  // Handler returns protobuf when Accept: application/x-protobuf
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  examvan::v1::CreateExamResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid CreateExamResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_EQ(pb.name(), "Ujian TDD Proto");
  EXPECT_EQ(pb.token(), "PBTEST01");
}

TEST(ProtobufHandlers, CreateExam_MultipartStillWorks) {
  setenv("EXAMVAN_R2_TESTMODE","1",1); // test harness tanpa R2 nyata: opt-in eksplisit
  std::string ct = "multipart/form-data; boundary=----TestPB";
  std::string body = "------TestPB\r\n"
    "Content-Disposition: form-data; name=\"name\"\r\n\r\nUjian Multipart\r\n"
    "------TestPB\r\n"
    "Content-Disposition: form-data; name=\"pdf_file\"; filename=\"soal.pdf\"\r\n"
    "Content-Type: application/pdf\r\n\r\n%PDF-1.4 fake\r\n%%EOF\r\n"
    "------TestPB--\r\n";
  Request req;
  req.method = "POST";
  req.headers["Content-Type"] = ct;
  req.body = body;
  auto res = handlers::admin::create_exam(req);
  EXPECT_NE(res.status, 415) << res.body;
}

TEST(ProtobufHandlers, UpdateExam_JsonStillWorks) {
  // Tanpa id, handler real memberi 400 (bukan no-op 200). Ini mengunci bahwa
  // update_exam sekarang memvalidasi input, bukan sukses palsu.
  auto res = handlers::admin::update_exam(Request{});
  EXPECT_EQ(res.status, 400);
}

TEST(ProtobufHandlers, DeleteExam_JsonStillWorks) {
  // Tanpa id, handler real memberi 400 (bukan no-op 200).
  auto res = handlers::admin::delete_exam(Request{});
  EXPECT_EQ(res.status, 400);
}

TEST(ProtobufHandlers, ExportXlsx_NoProtobufNeeded) {
  auto res = handlers::admin::export_xlsx(Request{});
  // Export XLSX nyata — tanpa header protobuf tetap 200 + file valid (zip).
  EXPECT_EQ(res.status, 200) << res.body.substr(0,100);
  EXPECT_NE(res.headers.at("Content-Type").find("spreadsheetml"), std::string::npos);
  EXPECT_EQ(res.body.substr(0,2), "PK");
}

// ======================================================================
// Admin: Vouchers
// ======================================================================

TEST(ProtobufHandlers, ListVouchers_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::admin::list_vouchers(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::VoucherList pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid VoucherList protobuf";
  EXPECT_TRUE(pb.success());
}

TEST(ProtobufHandlers, ListVouchers_JsonStillWorks) {
  Request req; req.method = "GET";
  auto res = handlers::admin::list_vouchers(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, RedeemVoucher_JsonStillWorks) {
  // handler bukan stub: redeem butuh DB nyata. Tanpa DATABASE_URL → 503
  // jujur (sukses palsu 200 dihapus); alur sukses diverifikasi smoke test PG.
  unsetenv("DATABASE_URL");
  Request req; req.method = "POST"; req.body = "code=ABC123";
  req.headers["X-Internal-Admin-Id"]="2"; // session valid; tanpa DB → 503 jujur
  auto res = handlers::admin::redeem_voucher(req);
  EXPECT_EQ(res.status, 503);
  EXPECT_NE(res.body.find("Database tidak tersedia"), std::string::npos);
}

TEST(ProtobufHandlers, RedeemVoucher_MissingCodeReturns400) {
  Request req; req.method = "POST"; req.body = "";
  auto res = handlers::admin::redeem_voucher(req);
  EXPECT_EQ(res.status, 400);
  EXPECT_NE(res.body.find("Silakan masukkan kode voucher"), std::string::npos);
}

// ======================================================================
// Admin: Settings
// ======================================================================

TEST(ProtobufHandlers, SettingsPage_ApiProtobufResponse) {
  auto req = pb_accept();
  req.path = "/admin/api/saas-settings";
  auto res = handlers::admin::settings_page(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::Settings pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid Settings protobuf";
  EXPECT_TRUE(pb.success());
}

TEST(ProtobufHandlers, SettingsPage_SystemAppsProtobufResponse) {
  auto req = pb_accept();
  req.path = "/admin/api/system-apps";
  auto res = handlers::admin::settings_page(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::VoucherList pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "system-apps should decode as VoucherList";
  EXPECT_TRUE(pb.success());
}

TEST(ProtobufHandlers, SettingsPage_JsonStillWorks) {
  Request req; req.method = "GET"; req.path = "/admin/api/saas-settings";
  auto res = handlers::admin::settings_page(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, UpdateSettings_JsonStillWorks) {
  // handler bukan stub lagi: partial update butuh minimal satu key yang hadir
  // (paritas Go pointer fields — body kosong → 400).
  Request req; req.body="{\"smtp_host\":\"smtp.test.local\"}";
  auto res = handlers::admin::update_settings(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
  auto empty = handlers::admin::update_settings(Request{});
  EXPECT_EQ(empty.status, 400);
}

// ======================================================================
// Admin: Pengawas
// ======================================================================

TEST(ProtobufHandlers, PengawasExams_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::admin::pengawas_exams(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::PengawasExamList pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid PengawasExamList protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_TRUE(pb.is_privileged());
}

TEST(ProtobufHandlers, PengawasExams_JsonStillWorks) {
  Request req; req.method = "GET";
  auto res = handlers::admin::pengawas_exams(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, PengawasSubmissions_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::admin::pengawas_submissions(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::PengawasSubmissionList pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid PengawasSubmissionList protobuf";
  EXPECT_TRUE(pb.success());
}

TEST(ProtobufHandlers, PengawasSubmissions_JsonStillWorks) {
  Request req; req.method = "GET"; req.params["exam_id"]="1";
  auto res = handlers::admin::pengawas_submissions(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, PendingApprovals_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::admin::pending_approvals(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::ApprovalList pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid ApprovalList protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_GE(pb.total(), 0);
}

TEST(ProtobufHandlers, PendingApprovals_JsonStillWorks) {
  Request req; req.method = "GET"; req.params["exam_id"]="1";
  auto res = handlers::admin::pending_approvals(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, SetApproval_JsonStillWorks) {
  Request req; req.params["exam_id"]="1"; req.params["mac_address"]="AA:BB:CC"; req.body="status=approved";
  auto res = handlers::admin::set_approval(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, GetAutoApprove_JsonStillWorks) {
  Request req; req.params["exam_id"]="1";
  auto res = handlers::admin::get_auto_approve(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, SetAutoApprove_JsonStillWorks) {
  Request req; req.params["exam_id"]="1"; req.body="enabled=true";
  auto res = handlers::admin::set_auto_approve(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

// ======================================================================
// Admin: Submissions
// ======================================================================

TEST(ProtobufHandlers, ListSubmissions_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::admin::list_submissions(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::SubmissionList pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid SubmissionList protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_GE(pb.total(), 0);
}

TEST(ProtobufHandlers, ListSubmissions_JsonStillWorks) {
  Request req; req.method = "GET";
  auto res = handlers::admin::list_submissions(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, SubmissionDetail_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::admin::submission_detail(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::SubmissionDetail pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid SubmissionDetail protobuf";
  EXPECT_TRUE(pb.success());
}

TEST(ProtobufHandlers, SubmissionDetail_JsonStillWorks) {
  Request req; req.method = "GET"; req.params["id"]="1";
  auto res = handlers::admin::submission_detail(req);
  // tanpa DB → jujur 404; dengan PG mengembalikan detail
  EXPECT_TRUE(res.status==200 || res.status==404);
  if(res.status==200) EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, QueueStatus_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::admin::queue_status(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::QueueStatusResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid QueueStatusResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_EQ(pb.pending(), 0);
  EXPECT_EQ(pb.failed(), 0);
}

TEST(ProtobufHandlers, QueueStatus_JsonStillWorks) {
  Request req; req.method = "GET";
  auto res = handlers::admin::queue_status(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, DeleteSubmission_JsonStillWorks) {
  Request req; req.params["id"]="1";
  auto res = handlers::admin::delete_submission(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

// ======================================================================
// API: Health
// ======================================================================

TEST(ProtobufHandlers, Health_ValidProtobufResponse) {
  auto req = pb_accept();
  req.path = "/api/health";
  auto res = handlers::api::health(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::HealthResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid HealthResponse protobuf";
  EXPECT_NE(pb.version().size(), 0u);
}

TEST(ProtobufHandlers, Health_JsonStillWorks) {
  Request req; req.method = "GET"; req.path = "/api/health";
  auto res = handlers::api::health(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

// ======================================================================
// API: Time
// ======================================================================

TEST(ProtobufHandlers, TimeHandler_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::api::time_handler(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::TimeResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid TimeResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_FALSE(pb.server_time().empty());
  EXPECT_EQ(pb.timezone(), "UTC");
}

TEST(ProtobufHandlers, TimeHandler_JsonStillWorks) {
  Request req; req.method = "GET";
  auto res = handlers::api::time_handler(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

// ======================================================================
// API: List Exams (student-facing)
// ======================================================================

TEST(ProtobufHandlers, ListExams_ValidProtobufResponse) {
  auto req = pb_accept();
  req.path = "/api/exams";
  auto res = handlers::api::list_exams(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::ListExamsResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid ListExamsResponse protobuf";
  EXPECT_TRUE(pb.success());
}

TEST(ProtobufHandlers, ListExams_JsonStillWorks) {
  Request req; req.method = "GET"; req.path = "/api/exams";
  auto res = handlers::api::list_exams(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

// ======================================================================
// API: Exam by Token
// ======================================================================

TEST(ProtobufHandlers, ExamByToken_ValidProtobufResponse) {
  std::string token=prepare_started_exam_for_api();
  ASSERT_FALSE(token.empty());
  auto req = pb_accept();
  req.params["token"] = token;
  auto res = handlers::api::exam_by_token(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::ExamByTokenResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid ExamByTokenResponse protobuf";
  EXPECT_TRUE(pb.success());
}

TEST(ProtobufHandlers, ExamByToken_InvalidTokenReturns404Protobuf) {
  // "x" is 1 char — fails is_valid_exam_token (min 6 alnum)
  auto req = pb_accept();
  req.params["token"] = "x";
  auto res = handlers::api::exam_by_token(req);
  EXPECT_EQ(res.status, 404);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  examvan::v1::ExamByTokenResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body));
  EXPECT_FALSE(pb.success());
}

TEST(ProtobufHandlers, ExamByToken_JsonStillWorks) {
  std::string token=prepare_started_exam_for_api();
  ASSERT_FALSE(token.empty());
  Request req; req.method = "GET"; req.params["token"] = token;
  auto res = handlers::api::exam_by_token(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"token\":\""+token+"\""), std::string::npos);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

// ======================================================================
// API: Request Approval
// ======================================================================

TEST(ProtobufHandlers, RequestApproval_ValidProtobufResponse) {
  int eid=prepare_started_exam_id_for_api();
  ASSERT_GT(eid,0);
  auto exam=store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  auto req = pb_accept();
  req.method = "POST";
  req.body="exam_id="+std::to_string(eid)+"&mac_address=aa:bb:cc&token="+exam->token;
  auto res = handlers::api::request_approval(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::RequestApprovalResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid RequestApprovalResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_EQ(pb.status(), "pending");
}

TEST(ProtobufHandlers, RequestApproval_JsonStillWorks) {
  int eid=prepare_started_exam_id_for_api();
  ASSERT_GT(eid,0);
  auto exam=store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  Request req; req.method = "POST";
  req.body="exam_id="+std::to_string(eid)+"&mac_address=aa:bb:cc&token="+exam->token;
  auto res = handlers::api::request_approval(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"pending\""), std::string::npos);
}

// ======================================================================
// API: Submit Exam
// ======================================================================

TEST(ProtobufHandlers, SubmitExam_ValidProtobufResponse) {
  int eid=prepare_started_exam_id_for_api();
  ASSERT_GT(eid,0);
  auto exam=store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  auto req = pb_accept();
  req.method = "POST";
  req.params["exam_id"] = std::to_string(eid);
  req.headers["X-Exam-Token"] = exam->token;
  auto res = handlers::api::submit_exam(req);
  EXPECT_EQ(res.status, 202);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::SubmitExamResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid SubmitExamResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_EQ(pb.status(), "queued");
}

TEST(ProtobufHandlers, SubmitExam_JsonStillWorks) {
  int eid=prepare_started_exam_id_for_api();
  ASSERT_GT(eid,0);
  auto exam=store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  Request req; req.method = "POST"; req.params["exam_id"] = std::to_string(eid);
  req.headers["X-Exam-Token"] = exam->token;
  auto res = handlers::api::submit_exam(req);
  EXPECT_EQ(res.status, 202);
  EXPECT_NE(res.body.find("\"status\":\"queued\""), std::string::npos);
}

// ======================================================================
// API: Exam Result
// ======================================================================

TEST(ProtobufHandlers, ExamResult_ValidProtobufResponse) {
  int eid=prepare_started_exam_id_for_api();
  ASSERT_GT(eid,0);
  auto req = pb_accept();
  req.params["exam_id"] = std::to_string(eid);
  auto res = handlers::api::exam_result(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::ExamResultResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid ExamResultResponse protobuf";
  EXPECT_TRUE(pb.success());
}

TEST(ProtobufHandlers, ExamResult_JsonStillWorks) {
  int eid=prepare_started_exam_id_for_api();
  ASSERT_GT(eid,0);
  Request req; req.params["exam_id"] = std::to_string(eid);
  auto res = handlers::api::exam_result(req);
  EXPECT_EQ(res.status, 200);
  // exam_result returns {exam_id, score} — no explicit "success" field
  EXPECT_NE(res.body.find("\"exam_id\":"+std::to_string(eid)), std::string::npos);
  EXPECT_NE(res.body.find("\"score\""), std::string::npos);
}

// ======================================================================
// API: Access Log
// ======================================================================

TEST(ProtobufHandlers, AccessLog_ValidProtobufResponse) {
  int eid=prepare_started_exam_id_for_api();
  ASSERT_GT(eid,0);
  auto exam=store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  auto req = pb_accept();
  req.method = "POST";
  req.params["exam_id"] = std::to_string(eid);
  req.headers["X-Exam-Token"] = exam->token;
  auto res = handlers::api::access_log(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::AccessLogResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid AccessLogResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_TRUE(pb.logged());
}

TEST(ProtobufHandlers, AccessLog_JsonStillWorks) {
  int eid=prepare_started_exam_id_for_api();
  ASSERT_GT(eid,0);
  auto exam=store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  Request req; req.method = "POST"; req.params["exam_id"] = std::to_string(eid);
  req.headers["X-Exam-Token"] = exam->token;
  auto res = handlers::api::access_log(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"logged\":true"), std::string::npos);
}

// ======================================================================
// API: Complete Exam
// ======================================================================

TEST(ProtobufHandlers, CompleteExam_ValidProtobufResponse) {
  int eid=prepare_started_exam_id_for_api();
  ASSERT_GT(eid,0);
  auto exam=store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  auto req = pb_accept();
  req.method = "POST";
  req.params["exam_id"] = std::to_string(eid);
  req.headers["X-Exam-Token"] = exam->token;
  req.body="mac_address=aa:bb";
  auto res = handlers::api::complete_exam(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::CompleteExamResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid CompleteExamResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_TRUE(pb.completed());
}

TEST(ProtobufHandlers, CompleteExam_JsonStillWorks) {
  int eid=prepare_started_exam_id_for_api();
  ASSERT_GT(eid,0);
  auto exam=store::active_store()->get_by_id(eid);
  ASSERT_TRUE(exam.has_value());
  Request req; req.method = "POST"; req.params["exam_id"] = std::to_string(eid);
  req.headers["X-Exam-Token"] = exam->token;
  req.body="mac_address=aa:bb";
  auto res = handlers::api::complete_exam(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"completed\":true"), std::string::npos);
}

// ======================================================================
// Handler README: no regression when Accept header is absent
// ======================================================================

// ======================================================================
// Webhook
// ======================================================================

TEST(ProtobufHandlers, Webhook_ValidProtobufResponse) {
  auto req = pb_accept();
  req.method = "POST";
  req.body = "some payload";
  auto res = handlers::api::webhook(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::WebhookResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid WebhookResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_EQ(pb.status(), "ok");
}

TEST(ProtobufHandlers, Webhook_JsonStillWorks) {
  // Payload tanpa sender/message → 200 status:false (paritas Go), bukan
  // {"ok":true} ack buta.
  Request req; req.method = "POST"; req.body = "p";
  auto res = handlers::api::webhook(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"status\":false"), std::string::npos);
  EXPECT_NE(res.body.find("Payload tidak lengkap"), std::string::npos);
}

TEST(ProtobufHandlers, Webhook_EmptyBodyReturns400) {
  auto req = pb_accept();
  req.method = "POST";
  req.body = "";
  auto res = handlers::api::webhook(req);
  EXPECT_EQ(res.status, 400);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
}

// ======================================================================
// Cek Hasil API
// ======================================================================

TEST(ProtobufHandlers, CekHasilApi_ValidProtobufResponse) {
  auto req = pb_accept();
  auto res = handlers::public_::cek_hasil_api(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_EQ(res.headers.at("Content-Type"), "application/x-protobuf");
  ASSERT_FALSE(res.body.empty());
  examvan::v1::CekHasilApiResponse pb;
  ASSERT_TRUE(pb.ParseFromString(res.body)) << "body is not valid CekHasilApiResponse protobuf";
  EXPECT_TRUE(pb.success());
  EXPECT_TRUE(pb.ok());
}

TEST(ProtobufHandlers, CekHasilApi_JsonStillWorks) {
  // Tanpa token → 404 jujur (dulu stub balas 200 {"ok":true} tanpa data).
  Request req;
  auto res = handlers::public_::cek_hasil_api(req);
  EXPECT_EQ(res.status, 404);
  EXPECT_NE(res.body.find("success\":false"), std::string::npos);
  // Token valid + hasil publik → kontrak lengkap (bukan {"ok":true}).
  handlers::public_::clear_exams_for_test();
  examvan::models::Exam e; e.token="TOKX1"; e.name="UAS"; e.public_results=1;
  handlers::public_::set_exam_for_test("TOKX1", e);
  req.params["token"]="TOKX1";
  auto ok=handlers::public_::cek_hasil_api(req);
  EXPECT_EQ(ok.status, 200);
  EXPECT_NE(ok.body.find("\"submissions\":["), std::string::npos);
  EXPECT_NE(ok.body.find("\"pagination\":{"), std::string::npos);
  handlers::public_::clear_exams_for_test();
}

// ======================================================================
// Handler README: no regression when Accept header is absent
// ======================================================================

TEST(ProtobufHandlers, NoAcceptHeader_DefaultsToJson) {
  Request req; req.method = "GET";
  auto res = handlers::admin::dashboard_stats(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}

TEST(ProtobufHandlers, AcceptWidcard_FallsToJson) {
  Request req; req.method = "GET";
  req.headers["Accept"] = "*/*";
  auto res = handlers::admin::dashboard_stats(req);
  EXPECT_EQ(res.status, 200);
  EXPECT_NE(res.body.find("\"success\":true"), std::string::npos);
}
