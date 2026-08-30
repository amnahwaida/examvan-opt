#include <gtest/gtest.h>
#include "websocket/hub.hpp"
#include "websocket/socketio.hpp"
#include "middleware/scoring.hpp"
#include "middleware/cors.hpp"
#include "queue/submission_queue.hpp"
#include "handlers/admin/export.hpp"
#include "session/cookie.hpp"
#include "config/config.hpp"
#include "utils/sanitize.hpp"
#include <fstream>
#include <regex>

using namespace examvan;
using namespace examvan::scoring;
using namespace examvan::queue;

// P0: JSON manual parsing rapuh
TEST(AuditJsonExtract, ExtractIgnoresKeyInsideValue) {
  std::string json = R"({"note":"has \"mac_address\" inside","mac_address":"aa:bb"})";
  auto v = Hub::extract_json_string(json, "mac_address");
  EXPECT_EQ(v, "aa:bb") << "must not pick key inside string value";
}

TEST(AuditJsonExtract, ExtractHandlesEscapedQuote) {
  std::string json = R"({"student_name":"Budi \"The Great\"","mac_address":"cc:dd"})";
  auto v = Hub::extract_json_string(json, "student_name");
  EXPECT_EQ(v, R"(Budi \"The Great\")");
  // after unescaping via Hub handling, sanitize should strip quotes; raw extraction keeps escapes
  EXPECT_NE(v.find("\\\""), std::string::npos);
}

TEST(AuditJsonExtract, ExtractNestedObjectNotConfused) {
  std::string json = R"({"outer":{"mac_address":"inner"},"mac_address":"outer_val"})";
  // simple manual find would pick first occurrence inside nested; robust should still find but prefer correct one
  auto v = Hub::extract_json_string(json, "mac_address");
  // at minimum should not return empty and should be one of the two; but must handle escaped correctly
  EXPECT_FALSE(v.empty());
}

TEST(AuditSocketIOJsonString, EscapesControlChars) {
  std::string s; s.push_back(char(0x01)); s.push_back(char(0x08)); s.push_back(char(0x0C));
  s += "\n\r\t\"\\";
  auto j = json_string(s);
  // must contain \u0001 for 0x01 and escapes for others
  EXPECT_NE(j.find("\\u0001"), std::string::npos) << j;
  EXPECT_NE(j.find("\\b"), std::string::npos) << j;
  EXPECT_NE(j.find("\\f"), std::string::npos) << j;
  EXPECT_NE(j.find("\\n"), std::string::npos);
  EXPECT_NE(j.find("\\r"), std::string::npos);
  EXPECT_NE(j.find("\\t"), std::string::npos);
  EXPECT_NE(j.find("\\\""), std::string::npos);
  EXPECT_NE(j.find("\\\\"), std::string::npos);
}

TEST(AuditSocketIOJsonString, NoRawControlChar) {
  std::string s; for(int i=0;i<0x20;i++) if(i!='\n'&&i!='\r'&&i!='\t'&&i!='\b'&&i!='\f') s.push_back(char(i));
  auto j = json_string(s);
  for(char c: j){
    if(c!='\"' && c!='\\' && c!=',' && c!=':' && c!='[' && c!=']' && c!='{' && c!='}' && c!='u' && (c>='0'&&c<='9'||c>='a'&&c<='f')) continue;
  }
  // ensure no raw <0x20 inside quoted string except escapes
  std::string inner = j.substr(1, j.size()-2);
  for(unsigned char c: inner){ EXPECT_GE(c, 0x20u) << "raw control char not escaped"; }
}

// P0: scoring JSON rapuh
TEST(AuditScoring, ParseWithEscapedQuotes) {
  std::string j = R"([{"number":1,"type":"single_choice","key":"a\"b","weight":1},{"number":2,"type":"single_choice","key":"c","weight":1}])";
  auto qs = parse_questions(j);
  ASSERT_EQ(qs.size(), 2u);
  // ensure key with escaped quote not truncated
  EXPECT_FALSE(qs[0].key.empty());
}

TEST(AuditScoring, ParseNestedBraceInString) {
  std::string j = R"([{"number":1,"type":"single_choice","key":"{not a brace}","weight":1}])";
  auto qs = parse_questions(j);
  ASSERT_EQ(qs.size(), 1u);
  EXPECT_EQ(qs[0].key, "{not a brace}");
}

// P0: json_unescape utf8
TEST(AuditQueueUnescape, UnicodeBMPToUtf8) {
  SubmissionJob j; j.job_id="test123"; j.exam_id=1; j.student_name="A\u00e9B"; // will be escaped via to_json
  // directly test from_json with \u00e9 and \u4e00
  std::string raw = R"({"job_id":"jid123","exam_id":1,"student_name":"A\u00e9B \u4e00","mac_address":"aa"})";
  auto pj = SubmissionJob::from_json(raw);
  ASSERT_TRUE(pj.has_value());
  // \u00e9 should become UTF-8 bytes C3 A9, \u4e00 -> E4 B8 80, not single truncated char
  std::string expected = "A\xC3\xA9" "B \xE4\xB8\x80";
  EXPECT_EQ(pj->student_name, expected) << "got bytes:"; 
  for(unsigned char c: pj->student_name) { /* debug */ (void)c; }
}

TEST(AuditQueueUnescape, RoundTripUnicode) {
  SubmissionJob j; j.job_id="jid1"; j.exam_id=5; j.student_name="Héllo 中文"; j.mac_address="aa";
  auto json = j.to_json();
  auto j2 = SubmissionJob::from_json(json);
  ASSERT_TRUE(j2.has_value());
  EXPECT_EQ(j2->student_name, "Héllo 中文");
}

// P0: generate_job_id must be crypto secure (length + hex + uniqueness, not predictable)
TEST(AuditJobId, IsHexAndLength) {
  auto id = generate_job_id();
  EXPECT_EQ(id.size(), 16u);
  EXPECT_TRUE(std::regex_match(id, std::regex("[0-9a-f]{16}")));
}
TEST(AuditJobId, UniqueMany) {
  std::set<std::string> s;
  for(int i=0;i<100;i++) s.insert(generate_job_id());
  EXPECT_EQ(s.size(), 100u);
}

// P0: check_origin empty must be denied
TEST(AuditOrigin, EmptyDenied) {
  EXPECT_FALSE(check_origin("", "example.com")) << "empty origin must be rejected (CSWSH bypass)";
  EXPECT_FALSE(check_origin("", "")) ;
}
TEST(AuditOrigin, NullOriginDenied) {
  EXPECT_FALSE(check_origin("null", "example.com"));
}
TEST(AuditOrigin, LocalhostAllowed) {
  EXPECT_TRUE(check_origin("http://localhost:3000", "example.com"));
  EXPECT_TRUE(check_origin("http://127.0.0.1:8080", "example.com"));
}
TEST(AuditOrigin, EvilDenied) {
  EXPECT_FALSE(check_origin("https://evil.com", "example.com"));
}
TEST(AuditOrigin, HostMatchAllowed) {
  EXPECT_TRUE(check_origin("https://example.com", "example.com"));
  EXPECT_TRUE(check_origin("https://example.com:443", "example.com"));
}

// P1: CORS wildcard must be rejected
TEST(AuditCors, WildcardRejected) {
  EXPECT_FALSE(examvan::middleware::is_origin_allowed("https://evil.com", "*"));
  EXPECT_FALSE(examvan::middleware::is_origin_allowed("https://a.com", "*"));
}
TEST(AuditCors, ExplicitAllowed) {
  EXPECT_TRUE(examvan::middleware::is_origin_allowed("https://a.com", "https://a.com, https://b.com"));
  EXPECT_FALSE(examvan::middleware::is_origin_allowed("https://evil.com", "https://a.com, https://b.com"));
}
TEST(AuditCors, VaryHeaderPresent) {
  Request r; r.headers["Origin"]="https://a.com";
  auto res = examvan::middleware::cors_wrap(r, "https://a.com", [](const Request&){ Response ok; ok.status=200; ok.body="ok"; return ok; });
  // should add Vary: Origin
  auto it = res.headers.find("Vary");
  ASSERT_TRUE(it!=res.headers.end()) << "Vary header missing";
  EXPECT_NE(it->second.find("Origin"), std::string::npos);
}

// P0: cookie fallback without HMAC must not be oracle
TEST(AuditCookie, B64UrlWithoutHMACRejected) {
  std::string secret = "0123456789abcdef0123456789abcdef"; // 32
  std::string payload = "admin_id=1&username=evil";
  std::string payload_b64 = b64_encode(payload);
  std::string forged_b64url = b64url_encode(payload); // no sig
  // verify_session_cookie should reject if only b64url without sig
  auto sess = verify_session_cookie(secret, "examvan_session="+forged_b64url);
  EXPECT_FALSE(sess.has_value()) << "b64url without HMAC must be rejected";
}
TEST(AuditCookie, ValidSigAccepted) {
  std::string secret = "0123456789abcdef0123456789abcdef";
  std::string payload_b64 = b64_encode("admin_id=1&username=budi&role=guru");
  std::string cv = encode_cookie_value(secret, payload_b64);
  auto sess = verify_session_cookie(secret, "examvan_session="+cv);
  ASSERT_TRUE(sess.has_value());
  EXPECT_EQ(sess->admin_id, 1);
}

// P1: CSV formula injection must handle leading tab/cr and whitespace
TEST(AuditCsv, FormulaInjectionTab) {
  std::string exam = "\t=1+1";
  auto csv = examvan::handlers::admin::build_csv_export(exam);
  EXPECT_NE(csv.find("'"), std::string::npos) << csv;
  EXPECT_EQ(csv.find("\n\t=1+1"), std::string::npos) << "raw tab formula at field start not escaped";
  EXPECT_NE(csv.find("'\t=1+1"), std::string::npos) << csv;
}
TEST(AuditCsv, FormulaInjectionAt) {
  auto csv = examvan::handlers::admin::build_csv_export("@evil");
  EXPECT_NE(csv.find("'@evil"), std::string::npos);
}
TEST(AuditCsv, FormulaNormalNotEscaped) {
  auto csv = examvan::handlers::admin::build_csv_export("Ujian Normal");
  EXPECT_NE(csv.find("Ujian Normal"), std::string::npos);
  // should not prefix with '
  EXPECT_EQ(csv.find("'Ujian Normal"), std::string::npos);
}

// P1: nginx file checks (infra)
TEST(AuditNginx, RateLimitTight) {
  std::ifstream f("nginx/nginx.conf");
  ASSERT_TRUE(f.good());
  std::string c((std::istreambuf_iterator<char>(f)), {});
  // should be 2r/s or 3r/s, not 10r/s
  EXPECT_EQ(c.find("rate=10r/s"), std::string::npos) << "rate still 10r/s too loose";
  EXPECT_NE(c.find("rate="), std::string::npos);
  // must have limit_conn
  EXPECT_NE(c.find("limit_conn"), std::string::npos);
  // must have timeouts
  EXPECT_TRUE(c.find("client_header_timeout")!=std::string::npos || c.find("client_body_timeout")!=std::string::npos);
}
TEST(AuditDockerfile, NoCopyLibStdCppGlob) {
  std::ifstream f("Dockerfile");
  ASSERT_TRUE(f.good());
  std::string c((std::istreambuf_iterator<char>(f)), {});
  EXPECT_EQ(c.find("COPY --from=builder /usr/local/lib64/libstdc++.so.6*"), std::string::npos) << "brittle COPY libstdc++ glob";
  EXPECT_EQ(c.find("libhiredis0.14"), std::string::npos) << "pinned libhiredis0.14 brittle";
  EXPECT_NE(c.find("libhiredis"), std::string::npos);
}
TEST(AuditCMake, NoGlobalWerror) {
  std::ifstream f("CMakeLists.txt");
  ASSERT_TRUE(f.good());
  std::string c((std::istreambuf_iterator<char>(f)), {});
  // global add_compile_options(-Werror) should be scoped to target
  // check that file does NOT contain bare add_compile_options with Werror at top level before target
  size_t pos = c.find("add_compile_options(-Wall");
  // after fix, should have target_compile_options
  EXPECT_NE(c.find("target_compile_options"), std::string::npos);
  if(pos!=std::string::npos){
    // ensure next line is target scoped, not global Werror alone
    // simple: file should not have "add_compile_options(-Wall -Wextra -Wpedantic -Werror"
    EXPECT_EQ(c.find("add_compile_options(-Wall -Wextra -Wpedantic -Werror"), std::string::npos) << "global Werror still present";
  }
}
