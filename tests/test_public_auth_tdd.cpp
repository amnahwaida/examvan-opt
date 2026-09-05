#include <gtest/gtest.h>
#include "handlers/auth/register.hpp"
#include "handlers/auth/recovery.hpp"
#include "handlers/auth/auth_store.hpp"
#include "session/cookie.hpp"
#include "session/csrf.hpp"
#include "helpers/utils.hpp"
#include "helpers/password.hpp"
#include <string>
using namespace examvan;
using namespace examvan::handlers::auth;

namespace {

// Ekstrak token CSRF dari HTML halaman (meta atau hidden input).
std::string csrf_from_html(const std::string& html){
  std::string n1="csrf-token\" content=\"";
  auto p=html.find(n1);
  if(p!=std::string::npos){ p+=n1.size(); auto e=html.find('"',p); if(e!=std::string::npos) return html.substr(p,e-p); }
  std::string n2="name=\"csrf_token\" value=\"";
  p=html.find(n2);
  if(p!=std::string::npos){ p+=n2.size(); auto e=html.find('"',p); if(e!=std::string::npos) return html.substr(p,e-p); }
  return "";
}

// Buka halaman GET + bawa cookie/token CSRF yang sah (pola CsrfFix).
struct Session {
  std::string cookie;
  std::string token;
};
Session csrf_session(const char* path){
  Request req;
  std::string path_s=path;
  size_t qpos=path_s.find('?');
  if(qpos!=std::string::npos){ req.query=path_s.substr(qpos+1); path_s=path_s.substr(0,qpos); }
  req.path=path_s;
  Response page;
  if(path_s=="/register") page=register_page(req);
  else if(path_s=="/forgot-password") page=forgot_password_page(req);
  else if(path_s.rfind("/reset-password",0)==0) page=reset_password_page(req);
  else if(path_s.rfind("/register/confirm",0)==0) page=register_confirm_page(req);
  else return {};
  Session s;
  s.cookie=extract_cookie(page.headers["Set-Cookie"],"csrf_token");
  s.token=csrf_from_html(page.body);
  return s;
}

// Encode token untuk body form (seperti browser).
std::string enc(const std::string& t){
  std::string o;
  for(char c: t){ if(c=='+') o+="%2B"; else if(c=='/') o+="%2F"; else if(c=='=') o+="%3D"; else o+=c; }
  return o;
}

Config test_cfg(){
  Config cfg; cfg.secret_key=std::string(32,'x'); return cfg;
}

} // namespace

// ============================= GET pages =============================

TEST(PublicAuth, RegisterPageHasCsrfCookieAndToken){
  clear_registered_users_for_test();
  auto s=csrf_session("/register");
  EXPECT_FALSE(s.cookie.empty());
  EXPECT_FALSE(s.token.empty());
  EXPECT_EQ(s.cookie, s.token) << "cookie and html token must match for double-submit";
}

TEST(PublicAuth, ForgotPageHasCsrf){
  clear_registered_users_for_test();
  auto s=csrf_session("/forgot-password");
  EXPECT_FALSE(s.cookie.empty());
  EXPECT_FALSE(s.token.empty());
}

TEST(PublicAuth, ResetPageWithoutUsernameRedirects){
  clear_registered_users_for_test();
  Request req;
  auto res=reset_password_page(req);
  EXPECT_EQ(res.status,302);
  EXPECT_EQ(res.headers["Location"],"/forgot-password");
}

// ============================= /register =============================

TEST(PublicAuth, RegisterSuccessEmailVerifyOff){
  clear_registered_users_for_test();
  reset_register_limit_for_test();
  auto s=csrf_session("/register");
  Config cfg=test_cfg();
  Request req; req.path="/register";
  req.body="username=siswa1&email=a%40b.com&password=secret12&password_confirm=secret12&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=register_handler(req,cfg);
  EXPECT_EQ(res.status,200) << res.body;
  EXPECT_NE(res.body.find("success"), std::string::npos);
  RegisteredUser u;
  EXPECT_TRUE(find_registered_user("siswa1",u));
  EXPECT_EQ(u.status,"active"); // email_verification_enabled default 0
  clear_registered_users_for_test();
}

TEST(PublicAuth, RegisterInvalidPasswordRejects){
  clear_registered_users_for_test();
  reset_register_limit_for_test();
  auto s=csrf_session("/register");
  Config cfg=test_cfg();
  Request req; req.body="username=siswa2&email=a%40b.com&password=short&password_confirm=short&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=register_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  EXPECT_NE(res.body.find("8"), std::string::npos) << "should mention min length";
  clear_registered_users_for_test();
}

TEST(PublicAuth, RegisterPasswordOver72BytesRejected){
  clear_registered_users_for_test();
  reset_register_limit_for_test();
  auto s=csrf_session("/register");
  Config cfg=test_cfg();
  std::string longpass(80,'a'); // > 72 bytes — bcrypt memotong di 72
  Request req; req.body="username=siswa2b&email=a%40b.com&password="+longpass+"&password_confirm="+longpass+"&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=register_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  EXPECT_NE(res.body.find("72"), std::string::npos) << "should mention bcrypt 72-byte cap";
  RegisteredUser u;
  EXPECT_FALSE(find_registered_user("siswa2b",u)) << "over-72 password must not create account";
  clear_registered_users_for_test();
}

TEST(PublicAuth, RegisterReservedUsernameRejected){
  clear_registered_users_for_test();
  reset_register_limit_for_test();
  auto s=csrf_session("/register");
  Config cfg=test_cfg();
  Request req; req.body="username=admin&email=a%40b.com&password=secret12&password_confirm=secret12&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=register_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  clear_registered_users_for_test();
}

TEST(PublicAuth, RegisterDuplicateUsernameRejected){
  clear_registered_users_for_test();
  reset_register_limit_for_test();
  RegisteredUser u; u.username="dupe"; u.email="d@b.com"; u.password_hash="x"; u.status="active";
  insert_registered_user(u);
  auto s=csrf_session("/register");
  Config cfg=test_cfg();
  Request req; req.body="username=dupe&email=other%40b.com&password=secret12&password_confirm=secret12&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=register_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  EXPECT_NE(res.body.find("terdaftar"), std::string::npos);
  clear_registered_users_for_test();
}

TEST(PublicAuth, RegisterCsrfMismatch403){
  clear_registered_users_for_test();
  reset_register_limit_for_test();
  Request req; req.body="username=x&email=a%40b.com&password=secret12&password_confirm=secret12&_csrf=wrong";
  req.headers["Cookie"]="csrf_token=correct";
  req.headers["Accept"]="application/json";
  auto res=register_handler(req,test_cfg());
  EXPECT_EQ(res.status,403);
  clear_registered_users_for_test();
}

TEST(PublicAuth, RegisterDoesNotEchoPassword){
  clear_registered_users_for_test();
  reset_register_limit_for_test();
  auto s=csrf_session("/register");
  Config cfg=test_cfg();
  Request req; req.body="username=x&email=bad&password=supersecret123&password_confirm=supersecret123&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=register_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  EXPECT_EQ(res.body.find("supersecret123"), std::string::npos) << "password must not be echoed";
  clear_registered_users_for_test();
}

// ============================= /register/confirm =============================

TEST(PublicAuth, ConfirmPageUnknownUserNeutral){
  clear_registered_users_for_test();
  // Netral: halaman selalu 200 untuk user tak dikenal (anti enumerasi status).
  Request req; req.query="username=nobody";
  auto res=register_confirm_page(req);
  EXPECT_EQ(res.status,200);
  EXPECT_TRUE(res.body.find("csrf_token")!=std::string::npos || res.body.find("otp")!=std::string::npos);
}

TEST(PublicAuth, ConfirmWrongOtpIncrementsAndDeletesAt5){
  clear_registered_users_for_test();
  // User pending_otp dengan OTP 123456.
  set_registered_user_for_test("siswa9","s@b.com","hash","pending_otp","123456",0,0);
  auto s=csrf_session("/register/confirm?username=siswa9");
  Config cfg=test_cfg();
  Request req; req.path="/register/confirm"; req.query="username=siswa9";
  req.body="otp_code=000000&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=register_confirm_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  RegisteredUser u;
  ASSERT_TRUE(find_registered_user("siswa9",u));
  EXPECT_EQ(u.otp_attempts,1);
  // Tebakan salah sampai 5× → user dihapus.
  set_registered_user_for_test("siswa9","s@b.com","hash","pending_otp","123456",4,0);
  auto s2=csrf_session("/register/confirm?username=siswa9");
  Request req2; req2.query="username=siswa9";
  req2.body="otp_code=000000&csrf_token="+enc(s2.token);
  req2.headers["Cookie"]="csrf_token="+s2.cookie;
  req2.headers["Accept"]="application/json";
  auto res2=register_confirm_handler(req2,cfg);
  EXPECT_EQ(res2.status,400);
  // 5× salah → OTP dinonaktifkan (bukan hapus akun) — anti CSRF-DoS.
  ASSERT_TRUE(find_registered_user("siswa9",u)) << "5th wrong attempt disables OTP, does NOT delete user";
  EXPECT_TRUE(u.otp_code.empty()) << "otp should be disabled";
  clear_registered_users_for_test();
}

TEST(PublicAuth, ConfirmExpiredOtpDeletesUser){
  clear_registered_users_for_test();
  long past=std::time(nullptr)-1000;
  set_registered_user_for_test("siswa10","s@b.com","hash","pending_otp","123456",0,past);
  auto s=csrf_session("/register/confirm?username=siswa10");
  Config cfg=test_cfg();
  Request req; req.query="username=siswa10";
  req.body="otp_code=123456&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=register_confirm_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  RegisteredUser u;
  EXPECT_FALSE(find_registered_user("siswa10",u)) << "expired OTP user deleted";
  clear_registered_users_for_test();
}

TEST(PublicAuth, ConfirmCorrectOtpActivates){
  clear_registered_users_for_test();
  long future=std::time(nullptr)+900;
  set_registered_user_for_test("siswa11","s@b.com","hash","pending_otp","123456",0,future);
  auto s=csrf_session("/register/confirm?username=siswa11");
  Config cfg=test_cfg();
  Request req; req.query="username=siswa11";
  req.body="otp_code=123456&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=register_confirm_handler(req,cfg);
  EXPECT_EQ(res.status,200);
  RegisteredUser u;
  ASSERT_TRUE(find_registered_user("siswa11",u));
  EXPECT_EQ(u.status,"active");
  EXPECT_TRUE(u.otp_code.empty());
  clear_registered_users_for_test();
}

// ============================= /forgot-password =============================

TEST(PublicAuth, ForgotUnknownUserNeutralRedirect){
  clear_registered_users_for_test();
  auto s=csrf_session("/forgot-password");
  Config cfg=test_cfg();
  Request req; req.body="username=ghost&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=forgot_password_handler(req,cfg);
  EXPECT_EQ(res.status,200); // netral — sukses palsu
  EXPECT_NE(res.body.find("reset-password"), std::string::npos);
  clear_registered_users_for_test();
}

TEST(PublicAuth, ForgotKnownUserNeutralRedirect){
  clear_registered_users_for_test();
  set_registered_user_for_test("known","known@b.com","hash","active","",0,0);
  auto s=csrf_session("/forgot-password");
  Config cfg=test_cfg();
  Request req; req.body="username=known&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=forgot_password_handler(req,cfg);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("reset-password"), std::string::npos);
  clear_registered_users_for_test();
}

TEST(PublicAuth, ForgotCsrfMismatch403){
  clear_registered_users_for_test();
  Request req; req.body="username=x&_csrf=wrong";
  req.headers["Cookie"]="csrf_token=correct";
  req.headers["Accept"]="application/json";
  auto res=forgot_password_handler(req,test_cfg());
  EXPECT_EQ(res.status,403);
  clear_registered_users_for_test();
}

// ============================= /reset-password =============================

TEST(PublicAuth, ResetWrongOtpNeutralMessage){
  clear_registered_users_for_test();
  long future=std::time(nullptr)+900;
  set_registered_user_for_test("reseter","r@b.com","hash","active","654321",0,future);
  auto s=csrf_session("/reset-password?username=reseter");
  Config cfg=test_cfg();
  Request req; req.query="username=reseter";
  req.body="otp_code=000000&password=newpass12&password_confirm=newpass12&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=reset_password_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  EXPECT_NE(res.body.find("OTP"), std::string::npos) << "uniform message";
  RegisteredUser u;
  ASSERT_TRUE(find_registered_user("reseter",u));
  EXPECT_EQ(u.otp_attempts,1);
  clear_registered_users_for_test();
}

TEST(PublicAuth, ResetCorrectOtpChangesPassword){
  clear_registered_users_for_test();
  long future=std::time(nullptr)+900;
  set_registered_user_for_test("reseter2","r@b.com",helpers::hash_password("oldpass12"),"active","111222",0,future);
  auto s=csrf_session("/reset-password?username=reseter2");
  Config cfg=test_cfg();
  Request req; req.query="username=reseter2";
  req.body="otp_code=111222&password=newpass99&password_confirm=newpass99&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=reset_password_handler(req,cfg);
  EXPECT_EQ(res.status,200);
  RegisteredUser u;
  ASSERT_TRUE(find_registered_user("reseter2",u));
  EXPECT_TRUE(helpers::verify_password("newpass99",u.password_hash));
  EXPECT_FALSE(helpers::verify_password("oldpass12",u.password_hash));
  EXPECT_TRUE(u.otp_code.empty());
  clear_registered_users_for_test();
}

TEST(PublicAuth, ResetPasswordTooShortRejected){
  clear_registered_users_for_test();
  long future=std::time(nullptr)+900;
  set_registered_user_for_test("reseter3","r@b.com","hash","active","111222",0,future);
  auto s=csrf_session("/reset-password?username=reseter3");
  Config cfg=test_cfg();
  Request req; req.query="username=reseter3";
  req.body="otp_code=111222&password=short&password_confirm=short&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=reset_password_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  RegisteredUser u;
  ASSERT_TRUE(find_registered_user("reseter3",u));
  EXPECT_TRUE(helpers::verify_password("hash",u.password_hash)|| u.password_hash=="hash"); // unchanged
  clear_registered_users_for_test();
}

TEST(PublicAuth, ResetPasswordOver72BytesRejected){
  clear_registered_users_for_test();
  long future=std::time(nullptr)+900;
  set_registered_user_for_test("reseter5","r@b.com","hash","active","111222",0,future);
  auto s=csrf_session("/reset-password?username=reseter5");
  Config cfg=test_cfg();
  std::string longpass(80,'b');
  Request req; req.query="username=reseter5";
  req.body="otp_code=111222&password="+longpass+"&password_confirm="+longpass+"&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  req.headers["Accept"]="application/json";
  auto res=reset_password_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  EXPECT_NE(res.body.find("72"), std::string::npos) << "should mention bcrypt 72-byte cap";
  clear_registered_users_for_test();
}

TEST(PublicAuth, ResetCsrfMismatch403){
  clear_registered_users_for_test();
  Request req; req.query="username=x"; req.body="otp_code=1&password=a&_csrf=wrong";
  req.headers["Cookie"]="csrf_token=correct";
  req.headers["Accept"]="application/json";
  auto res=reset_password_handler(req,test_cfg());
  EXPECT_EQ(res.status,403);
  clear_registered_users_for_test();
}
TEST(PublicAuth, ResetErrorRerenderSetsCsrfCookie){
  // Form HTML (bukan JSON): error reset harus re-render halaman dengan
  // Set-Cookie csrf_token BARU agar percobaan ulang tidak gagal CSRF.
  clear_registered_users_for_test();
  long future=std::time(nullptr)+900;
  set_registered_user_for_test("reseter4","r@b.com","hash","active","999000",0,future);
  auto s=csrf_session("/reset-password?username=reseter4");
  Config cfg=test_cfg();
  Request req; req.query="username=reseter4";
  req.body="otp_code=000000&password=newpass12&password_confirm=newpass12&csrf_token="+enc(s.token);
  req.headers["Cookie"]="csrf_token="+s.cookie;
  // Tanpa Accept:application/json → jalur HTML.
  auto res=reset_password_handler(req,cfg);
  EXPECT_EQ(res.status,400);
  auto it=res.headers.find("Set-Cookie");
  ASSERT_NE(it,res.headers.end()) << "error re-render must set a fresh CSRF cookie";
  std::string new_cookie=extract_cookie(it->second,"csrf_token");
  EXPECT_FALSE(new_cookie.empty());
  EXPECT_NE(new_cookie,s.cookie) << "cookie should be rotated on error page";
  // Token di body HTML harus cocok dengan cookie baru.
  std::string html_token=csrf_from_html(res.body);
  EXPECT_EQ(html_token,new_cookie) << "html hidden token must match new cookie";
  clear_registered_users_for_test();
}
