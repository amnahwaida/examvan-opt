#include <gtest/gtest.h>
#include "config/config.hpp"
#include "middleware/cors.hpp"
#include "middleware/turnstile.hpp"
#include "middleware/scoring.hpp"
#include "queue/submission_queue.hpp"
#include "handlers/admin/export.hpp"
#include "handlers/r2/r2.hpp"
#include "session/cookie.hpp"
using namespace examvan;

TEST(P0_Config, SecretLengthRejectsShort) {
  Config c;
  c.secret_key = "short";
  c.admin_user = "admin";
  c.admin_pass = "pass";
  c.r2_access_key = "k";
  c.r2_secret_key = "s";
  c.r2_endpoint = "https://e";
  EXPECT_THROW(c.validate(), std::runtime_error);
}

TEST(P0_Config, SecretLengthAcceptsLongEnough) {
  Config c;
  c.secret_key = std::string(32,'a');
  c.admin_user = "admin";
  c.admin_pass = "pass";
  c.r2_access_key = "k";
  c.r2_secret_key = "s";
  c.r2_endpoint = "https://e";
  EXPECT_NO_THROW(c.validate());
}

TEST(P0_Turnstile, RejectsInvalidToken) {
  EXPECT_FALSE(middleware::verify_turnstile("invalid-random-token-xyz", "some-secret", "1.2.3.4"));
  EXPECT_FALSE(middleware::verify_turnstile("definitely-not-valid", "secret", "8.8.8.8"));
}

TEST(P0_Turnstile, AllowsBypassStill) {
  EXPECT_TRUE(middleware::verify_turnstile("test-bypass-token", "secret", "1.1.1.1"));
  EXPECT_FALSE(middleware::verify_turnstile("", "secret", "1.1.1.1"));
}

TEST(P0_Cors, EmptyCsvDeniesAll) {
  EXPECT_FALSE(middleware::is_origin_allowed("https://evil.com", ""));
  EXPECT_FALSE(middleware::is_origin_allowed("https://examvan.example.com", ""));
  EXPECT_FALSE(middleware::is_origin_allowed("http://localhost:5000", ""));
}

TEST(P0_Cors, WhitelistWorks) {
  std::string csv="https://a.com, https://b.com";
  EXPECT_TRUE(middleware::is_origin_allowed("https://a.com", csv));
  EXPECT_FALSE(middleware::is_origin_allowed("https://evil.com", csv));
}

TEST(P0_Queue, JsonEscapesSpecialChars) {
  queue::SubmissionJob j;
  j.job_id="abc123";
  j.exam_id=1;
  j.student_name="A\"B\\C\nD\tE";
  j.exam_number="12\"34";
  j.student_class="X\\Y";
  j.mac_address="AA:BB";
  std::string s=j.to_json();
  EXPECT_NE(s.find("\\\""), std::string::npos) << s;
  EXPECT_NE(s.find("\\\\"), std::string::npos) << s;
  EXPECT_EQ(s.find("\n"), std::string::npos) << "raw newline must be escaped";
  EXPECT_EQ(s.find("\t"), std::string::npos) << "raw tab must be escaped";
  auto parsed=queue::SubmissionJob::from_json(s);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->student_name, "A\"B\\C\nD\tE");
}

TEST(P0_Queue, JsonInjectionNotBreakStructure) {
  queue::SubmissionJob j;
  j.job_id="jid";
  j.exam_id=7;
  j.student_name="\",\"injected\":1,\"x\":\"";
  std::string s=j.to_json();
  auto p=queue::SubmissionJob::from_json(s);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->student_name, j.student_name);
  EXPECT_EQ(p->exam_id, 7);
}

TEST(P0_Session, B64DecodePreservesBinary) {
  std::string bin("hello\0world", 11);
  auto enc=b64_encode(bin);
  auto dec=b64_decode(enc);
  EXPECT_EQ(dec.size(), bin.size());
  EXPECT_EQ(dec, bin);
}

TEST(P0_Session, B64DecodeNoSpuriousTrim) {
  std::string bin2("\0\0\0",3);
  auto enc=b64_encode(bin2);
  auto dec=b64_decode(enc);
  EXPECT_EQ(dec.size(), bin2.size());
}

TEST(P0_R2, PresignCredentialEncoding) {
  r2::R2Config c{"AKIAEXAMPLE","wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY","https://example.r2.cloudflarestorage.com","mybucket"};
  auto url=r2::presign_url(c, "exams/5/file.pdf", 3600);
  ASSERT_FALSE(url.empty());
  EXPECT_NE(url.find("X-Amz-Algorithm=AWS4-HMAC-SHA256"), std::string::npos) << url;
  EXPECT_NE(url.find("X-Amz-Credential="), std::string::npos) << url;
  EXPECT_NE(url.find("X-Amz-Date="), std::string::npos) << url;
  EXPECT_NE(url.find("X-Amz-Expires=3600"), std::string::npos) << url;
  EXPECT_NE(url.find("X-Amz-SignedHeaders=host"), std::string::npos) << url;
  EXPECT_NE(url.find("%2F"), std::string::npos) << "credential must be url-encoded, got: " << url;
  EXPECT_EQ(url.find("/auto/s3/"), std::string::npos) << "must not use placeholder auto region: " << url;
  auto pos=url.find("X-Amz-Signature=");
  ASSERT_NE(pos, std::string::npos);
  std::string sig=url.substr(pos+16,64);
  EXPECT_EQ(sig.size(), 64u);
  for(char ch: sig) EXPECT_TRUE(isxdigit((unsigned char)ch)) << sig;
  EXPECT_NE(url.find("X-Amz-Signature="), url.rfind("X-Amz-Signature=") + 1);
}

TEST(P0_Export, XlsxIsValidZip) {
  auto x=handlers::admin::build_xlsx_placeholder("UAS");
  ASSERT_GE(x.size(), 4u);
  EXPECT_EQ(x.substr(0,2), "PK") << "XLSX must be ZIP";
  EXPECT_NE(x.find("[Content_Types].xml"), std::string::npos) << "missing content types";
  EXPECT_NE(x.find("xl/workbook.xml"), std::string::npos) << "missing workbook";
  EXPECT_EQ(x.find("placeholder"), std::string::npos) << "should not contain placeholder text";
  EXPECT_GT(x.size(), 500u) << "valid xlsx is larger than placeholder";
}

TEST(P0_Scoring, ParseDoesNotConfuseNumberInString) {
  std::string j=R"([{"number":1,"type":"single_choice","label":" trick \"number\":999 inside","weight":1}])";
  auto qs=scoring::parse_questions(j);
  EXPECT_EQ(qs.size(), 1u) << "naive string search parsed fake number inside label";
}

TEST(P0_Scoring, ParseMultipleRealQuestions) {
  std::string j=R"([{"number":1,"type":"single_choice","weight":2},{"number":2,"type":"true_false","weight":3}])";
  auto qs=scoring::parse_questions(j);
  ASSERT_EQ(qs.size(), 2u);
  EXPECT_EQ(qs[0].number, 1);
  EXPECT_EQ(qs[1].number, 2);
}
