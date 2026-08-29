#include <gtest/gtest.h>
#include "handlers/admin/export.hpp"
#include "websocket/hub.hpp"
#include "websocket/socketio.hpp"
#include "jobs/jobs.hpp"
#include "helpers/utils.hpp"
#include "config/config.hpp"
#include "redis/client.hpp"
#include "session/cookie.hpp"
#include <fstream>
#include <string>
using namespace examvan;

TEST(P2_Export, CsvInjectionEscaped) {
  auto csv = handlers::admin::build_csv_export("=cmd|' /C calc'!A0");
  EXPECT_EQ(csv.find("\n=cmd"), std::string::npos) << "CSV injection not escaped: " << csv;
  EXPECT_NE(csv.find("'=cmd"), std::string::npos) << "should prefix with single quote: " << csv;
}

TEST(P2_Export, XlsxContainsExamNameSafely) {
  auto x = handlers::admin::build_xlsx_placeholder("Test\"Exam");
  EXPECT_NE(x.find("Test"), std::string::npos);
  EXPECT_EQ(x.find("placeholder"), std::string::npos);
  EXPECT_GT(x.size(), 1000u);
}

TEST(P2_Jobs, ExpiryJobShouldBeImplemented) {
  std::ifstream f("src/jobs/jobs.cpp");
  ASSERT_TRUE(f.is_open());
  std::string content((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(content.find("DELETE"), std::string::npos) << "run_expiry_job still empty, should contain DELETE SQL";
  EXPECT_NE(content.find("try_acquire"), std::string::npos) << "should use Redis SET NX EX lock";
}

TEST(P2_Jobs, RetentionJobShouldBeImplemented) {
  std::ifstream f("src/jobs/jobs.cpp");
  std::string c((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(c.find("access_log"), std::string::npos) << "retention job should touch access_log";
}

TEST(P2_Hub, PrivilegedGateBlocksNonPrivilegedHeartbeat) {
  bool redis_set_called=false;
  Hub hub([&](auto,auto){ redis_set_called=true; }, nullptr, nullptr);
  auto client = std::make_shared<Client>();
  client->room="1"; client->privileged=false;
  hub.add_client(client);
  hub.handle_message(client, "42[\"heartbeat\",{\"mac_address\":\"AA:BB:CC:DD:EE:FF\"}]");
  EXPECT_FALSE(redis_set_called) << "non-privileged heartbeat should not trigger redis_set";
  EXPECT_EQ(hub.room_size("1"), 1u);
}

TEST(P2_Hub, HeartbeatSanitizesXSS) {
  std::string payload = R"({"mac_address":"AA:BB:CC:DD:EE:FF","student_name":"<script>alert(1)</script>"})";
  bool got_broadcast=false;
  Hub hub(nullptr,nullptr,[&](auto s){ got_broadcast=true; EXPECT_EQ(s.find("<script>"), std::string::npos); });
  auto c=std::make_shared<Client>(); c->room="5"; c->privileged=true;
  hub.add_client(c);
  auto other=std::make_shared<Client>(); other->room="5"; other->privileged=false;
  hub.add_client(other);
  hub.handle_message(c, marshal_socketio("heartbeat",payload));
  EXPECT_TRUE(other->send_queue.size()>0 || got_broadcast);
  if(other->send_queue.size()>0){
    auto msg=other->send_queue.front();
    EXPECT_EQ(msg.find("<script>"), std::string::npos) << msg;
  }
}

TEST(P2_Config, DatabaseMaxConnsEnforced) {
  Config c; c.secret_key=std::string(32,'x'); c.admin_user="u"; c.admin_pass="p"; c.r2_access_key="k"; c.r2_secret_key="s"; c.r2_endpoint="https://e";
  c.database_max_conns=200;
  EXPECT_THROW(c.validate(), std::runtime_error);
  c.database_max_conns=60; EXPECT_NO_THROW(c.validate());
}

TEST(P2_Config, PortRangeEnforced) {
  Config c; c.secret_key=std::string(32,'x'); c.admin_user="u"; c.admin_pass="p"; c.r2_access_key="k"; c.r2_secret_key="s"; c.r2_endpoint="https://e";
  c.port=99999; EXPECT_THROW(c.validate(), std::runtime_error);
  c.port=5000; EXPECT_NO_THROW(c.validate());
}

TEST(P2_Session, DualKeyFallback) {
  std::string cur=std::string(32,'a'), prev=std::string(32,'b');
  std::string payload=b64_encode("admin_id=99&username=test");
  auto cookie_prev=encode_cookie_value(prev, payload);
  auto r1=verify_session_cookie(cur, cookie_prev);
  EXPECT_FALSE(r1.has_value()) << "cur should not decode prev cookie";
  auto r2=verify_session_cookie_dual(cur, prev, cookie_prev);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2->admin_id, 99);
}
