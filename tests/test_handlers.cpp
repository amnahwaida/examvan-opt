#include <gtest/gtest.h>
#include "http/router.hpp"
#include "http/handlers.hpp"
using namespace examvan;
TEST(Handlers, Health) {
  Config cfg; cfg.version="2.7.2";
  Router r; register_routes(r,cfg);
  Request req; req.method="GET"; req.path="/api/health";
  auto res=r.dispatch(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("healthy"), std::string::npos);
}
TEST(Handlers, NotFound) {
  Config cfg; Router r; register_routes(r,cfg);
  Request req; req.method="GET"; req.path="/nope";
  EXPECT_EQ(r.dispatch(req).status,404);
}
TEST(Handlers, WSRoute) {
  // M7: stub GET /ws/:room_id dihapus — jalur WS asli lewat server (uWS/posix)
  // yang memeriksa Origin, bukan router HTTP. GET /ws/... biasa → 404.
  Config cfg; Router r; register_routes(r,cfg);
  Request req; req.method="GET"; req.path="/ws/123";
  auto res=r.dispatch(req);
  EXPECT_EQ(res.status,404);
}
