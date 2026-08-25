#include <gtest/gtest.h>
#include "handlers/api/exams.hpp"
#include "http/router.hpp"
#include "http/router_full.hpp"
#include "config/config.hpp"
using namespace examvan;

TEST(Api, Health) {
  Request req; req.method="GET"; req.path="/api/health";
  auto res=handlers::api::health(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("required_app_version"), std::string::npos);
}

TEST(Api, VersionGate) {
  Request req; req.method="GET"; req.path="/api/exams";
  req.headers["X-App-Version"]="1.0.0";
  auto res=handlers::api::list_exams(req);
  EXPECT_EQ(res.status,426);
  req.headers["X-App-Version"]="2.7.2";
  EXPECT_EQ(handlers::api::list_exams(req).status,200);
}

TEST(Api, ExamByTokenValid) {
  Request req; req.params["token"]="ABCDEFGH";
  EXPECT_EQ(handlers::api::exam_by_token(req).status,200);
  req.params["token"]="bad token!";
  EXPECT_EQ(handlers::api::exam_by_token(req).status,404);
}

TEST(Api, SubmitQueued) {
  Request req; EXPECT_EQ(handlers::api::submit_exam(req).status,202);
}

TEST(Api, FullRouterHas40Routes) {
  Config cfg; Router r; register_full_routes(r,cfg);
  EXPECT_GE(r.routes().size(), 30u);
  Request req; req.method="GET"; req.path="/api/health";
  EXPECT_EQ(r.dispatch(req).status,200);
  req.path="/api/nonexistent/xyz/missing"; EXPECT_EQ(r.dispatch(req).status,404);
}

TEST(Api, PresignPdfRedirect) {
  Request req; req.params["exam_id"]="5";
  auto res=handlers::api::exam_pdf(req);
  EXPECT_EQ(res.status,302);
  EXPECT_NE(res.headers["Location"].find("5"), std::string::npos);
}
