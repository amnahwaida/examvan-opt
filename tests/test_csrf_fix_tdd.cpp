#include <gtest/gtest.h>
#include "handlers/auth/login.hpp"
#include "handlers/auth/logout.hpp"
#include "session/cookie.hpp"
#include "session/csrf.hpp"
#include "helpers/utils.hpp"
#include <fstream>
#include <string>
using namespace examvan;
using namespace examvan::handlers::auth;

static std::string extract_csrf_from_html(const std::string& html){
  std::string n1="csrf-token\" content=\"";
  auto p=html.find(n1);
  if(p!=std::string::npos){
    p+=n1.size();
    auto e=html.find('"',p);
    if(e!=std::string::npos) return html.substr(p,e-p);
  }
  std::string n2="name=\"csrf_token\" value=\"";
  p=html.find(n2);
  if(p!=std::string::npos){ p+=n2.size(); auto e=html.find('"',p); if(e!=std::string::npos) return html.substr(p,e-p); }
  std::string n3="name=\"_csrf\" value=\"";
  p=html.find(n3);
  if(p!=std::string::npos){ p+=n3.size(); auto e=html.find('"',p); if(e!=std::string::npos) return html.substr(p,e-p); }
  return "";
}

TEST(CsrfFix, LoginPageReplacesAllPlaceholders){
  Request req;
  auto res=login_page(req);
  ASSERT_EQ(res.status,200);
  EXPECT_EQ(res.body.find("{{.csrf_token}}"), std::string::npos) << "template placeholder not replaced";
  EXPECT_EQ(res.body.find("{{ .csrf_token }}"), std::string::npos);
  EXPECT_EQ(res.body.find("CSRF_PLACEHOLDER"), std::string::npos);
  std::string token=extract_csrf_from_html(res.body);
  EXPECT_FALSE(token.empty()) << "no csrf in html";
  auto it=res.headers.find("Set-Cookie");
  ASSERT_NE(it,res.headers.end());
  std::string ck=extract_cookie(it->second,"csrf_token");
  EXPECT_FALSE(ck.empty());
  EXPECT_EQ(token, ck) << "html token must match cookie";
  int count=0; size_t pos=0; while((pos=res.body.find(token,pos))!=std::string::npos){ count++; pos+=token.size(); if(count>10) break; }
  EXPECT_GE(count,2) << "token should appear at least in meta and hidden input (found "<<count<<") body:"<<res.body.substr(0,800);
}

TEST(CsrfFix, LoginFormCsrfTokenField){
  clear_users_for_test(); set_user_for_test("guru","pass123","guru");
  Config cfg; cfg.secret_key=std::string(32,'x');
  auto page=login_page(Request{});
  std::string ck=extract_cookie(page.headers["Set-Cookie"],"csrf_token");
  ASSERT_FALSE(ck.empty());
  std::string csrf=extract_csrf_from_html(page.body);
  ASSERT_FALSE(csrf.empty());
  Request req; req.body="username=guru&password=pass123&csrf_token="+helpers::url_decode(csrf);
  // Simulate browser encoding: need to encode token for form body
  std::string enc; for(char c: csrf){ if(c=='+') enc+="%2B"; else if(c=='/') enc+="%2F"; else if(c=='=') enc+="%3D"; else enc+=c; }
  req.body="username=guru&password=pass123&csrf_token="+enc;
  req.headers["Cookie"]="csrf_token="+ck;
  req.headers["Accept"]="application/json";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,200) << res.body << " csrf cookie="<<ck<<" html="<<csrf;
  clear_users_for_test();
}

TEST(CsrfFix, LoginUnderscoreCsrfField){
  clear_users_for_test(); set_user_for_test("guru","pass123","guru");
  Config cfg; cfg.secret_key=std::string(32,'x');
  auto page=login_page(Request{});
  std::string ck=extract_cookie(page.headers["Set-Cookie"],"csrf_token");
  std::string csrf=extract_csrf_from_html(page.body);
  std::string enc; for(char c: csrf){ if(c=='+') enc+="%2B"; else if(c=='/') enc+="%2F"; else if(c=='=') enc+="%3D"; else enc+=c; }
  Request req; req.body="username=guru&password=pass123&_csrf="+enc;
  req.headers["Cookie"]="csrf_token="+ck;
  req.headers["Accept"]="application/json";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,200) << res.body;
  clear_users_for_test();
}

TEST(CsrfFix, LoginHeaderXCsrfToken){
  clear_users_for_test(); set_user_for_test("guru","pass123","guru");
  Config cfg; cfg.secret_key=std::string(32,'x');
  auto page=login_page(Request{});
  std::string ck=extract_cookie(page.headers["Set-Cookie"],"csrf_token");
  std::string csrf=extract_csrf_from_html(page.body);
  Request req; req.body="username=guru&password=pass123";
  req.headers["Cookie"]="csrf_token="+ck;
  req.headers["X-CSRF-Token"]=csrf;
  req.headers["Accept"]="application/json";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,200) << res.body;
  clear_users_for_test();
}

TEST(CsrfFix, LoginHeaderCaseInsensitive){
  clear_users_for_test(); set_user_for_test("guru","pass123","guru");
  Config cfg; cfg.secret_key=std::string(32,'x');
  auto page=login_page(Request{});
  std::string ck=extract_cookie(page.headers["Set-Cookie"],"csrf_token");
  std::string csrf=extract_csrf_from_html(page.body);
  Request req; req.body="username=guru&password=pass123";
  req.headers["Cookie"]="csrf_token="+ck;
  req.headers["x-csrf-token"]=csrf;
  req.headers["Accept"]="application/json";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,200) << "lowercase header should be accepted " << res.body;
  clear_users_for_test();
}

TEST(CsrfFix, LoginJsonBody){
  clear_users_for_test(); set_user_for_test("guru","pass123","guru");
  Config cfg; cfg.secret_key=std::string(32,'x');
  auto page=login_page(Request{});
  std::string ck=extract_cookie(page.headers["Set-Cookie"],"csrf_token");
  std::string csrf=extract_csrf_from_html(page.body);
  Request req;
  req.body="{\"username\":\"guru\",\"password\":\"pass123\",\"_csrf\":\""+csrf+"\"}";
  req.headers["Cookie"]="csrf_token="+ck;
  req.headers["Content-Type"]="application/json";
  req.headers["Accept"]="application/json";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,200) << "JSON body should be accepted " << res.body;
  clear_users_for_test();
}

TEST(CsrfFix, LoginTokenWithSpecialChars){
  clear_users_for_test(); set_user_for_test("guru","p","guru");
  Config cfg; cfg.secret_key=std::string(32,'x');
  std::string tok="abc+def/ghi==";
  Request req; req.body="username=guru&password=p&_csrf="+std::string("abc%2Bdef%2Fghi%3D%3D");
  req.headers["Cookie"]="csrf_token="+tok;
  req.headers["Accept"]="application/json";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,200) << res.body << " token with +/=";
  clear_users_for_test();
}

TEST(CsrfFix, LoginMismatchFails){
  clear_users_for_test(); set_user_for_test("guru","pass123","guru");
  Config cfg; cfg.secret_key=std::string(32,'x');
  Request req; req.body="username=guru&password=pass123&_csrf=wrong";
  req.headers["Cookie"]="csrf_token=correct-token";
  req.headers["Accept"]="application/json";
  auto res=login_handler(req,cfg);
  EXPECT_EQ(res.status,403);
  clear_users_for_test();
}

TEST(CsrfFix, ServerForwardsHeaders){
  std::ifstream f("src/server/server.cpp");
  ASSERT_TRUE(f.good());
  std::string c((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(c.find("X-CSRF-Token"), std::string::npos) << "server must forward X-CSRF-Token header";
  EXPECT_NE(c.find("X-Requested-With"), std::string::npos);
  EXPECT_NE(c.find("Accept"), std::string::npos);
  EXPECT_NE(c.find("Content-Type"), std::string::npos);
}

TEST(CsrfFix, LogoutCsrf){
  Request req; req.body="_csrf=test-csrf-token";
  req.headers["Cookie"]="csrf_token=test-csrf-token";
  auto res=logout_handler(req);
  EXPECT_EQ(res.status,200) << res.body;
  req.headers["Cookie"]="csrf_token=other";
  auto res2=logout_handler(req);
  EXPECT_EQ(res2.status,403);
}
