#include <gtest/gtest.h>
#include "websocket/hub.hpp"
#include "websocket/socketio.hpp"
using namespace examvan;

TEST(Hub, RoomJoinLeave) {
  Hub h;
  auto c1=std::make_shared<Client>(); c1->room="1"; c1->id="a";
  auto c2=std::make_shared<Client>(); c2->room="1"; c2->id="b";
  h.add_client(c1); h.add_client(c2);
  EXPECT_EQ(h.room_size("1"), 2u);
  h.remove_client(c1);
  EXPECT_EQ(h.room_size("1"), 1u);
  h.remove_client(c2);
  EXPECT_EQ(h.room_size("1"), 0u);
}

TEST(Hub, BroadcastToRoom) {
  Hub h;
  auto c=std::make_shared<Client>(); c->room="5"; c->id="x";
  h.add_client(c);
  h.broadcast_to_room("5","student_update","{\"event\":\"heartbeat\"}");
  ASSERT_FALSE(c->send_queue.empty());
  auto msg=c->send_queue.front();
  auto p=parse_socketio(msg);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->event,"student_update");
}

TEST(Hub, PrivilegedGateHeartbeat) {
  int set_calls=0;
  Hub h([&](auto,auto){ set_calls++; },nullptr,nullptr);
  auto c=std::make_shared<Client>(); c->room="10"; c->privileged=false; c->id="u";
  h.add_client(c);
  auto raw=marshal_socketio("heartbeat","{\"mac_address\":\"aa:bb:cc:dd:ee:ff\"}");
  h.handle_message(c, raw);
  EXPECT_EQ(set_calls,0);
  EXPECT_TRUE(c->send_queue.empty());
}

TEST(Hub, PrivilegedHeartbeatAllowed) {
  int set_calls=0; std::string last_key;
  Hub h([&](auto k,auto v){ set_calls++; last_key=k; },nullptr,[](auto){});
  auto c=std::make_shared<Client>(); c->room="10"; c->privileged=true; c->id="p";
  h.add_client(c);
  auto raw=marshal_socketio("heartbeat","{\"mac_address\":\"aa:bb\",\"student_name\":\"Budi\"}");
  h.handle_message(c, raw);
  EXPECT_EQ(set_calls,1);
  EXPECT_NE(last_key.find("heartbeat:10:"), std::string::npos);
  EXPECT_FALSE(c->send_queue.empty());
}

TEST(Hub, ExamCompletedRemovesHeartbeat) {
  int del_calls=0;
  Hub h(nullptr,[&](auto){del_calls++;},nullptr);
  auto c=std::make_shared<Client>(); c->room="7"; c->privileged=true;
  h.add_client(c);
  h.handle_message(c, marshal_socketio("exam_completed","{\"mac_address\":\"aa\"}"));
  EXPECT_EQ(del_calls,1);
}

TEST(Hub, PingPong) {
  Hub h;
  auto c=std::make_shared<Client>(); c->room="1"; c->privileged=false;
  h.add_client(c);
  h.handle_message(c, marshal_socketio("ping","null"));
  ASSERT_FALSE(c->send_queue.empty());
  auto p=parse_socketio(c->send_queue.front());
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->event,"pong");
}

TEST(Hub, BackpressureDrop) {
  Hub h;
  auto c=std::make_shared<Client>(); c->room="9";
  for(size_t i=0;i<Client::max_queue;i++) c->try_send("x");
  EXPECT_FALSE(c->try_send("overflow"));
  h.add_client(c);
  h.broadcast_to_room("9","ev","{}");
  EXPECT_EQ(h.room_size("9"), 0u);
}

TEST(Hub, OriginCheck) {
  EXPECT_FALSE(check_origin("","example.com"));
  EXPECT_TRUE(check_origin("https://example.com","example.com"));
  EXPECT_TRUE(check_origin("http://localhost:3000","example.com"));
  EXPECT_FALSE(check_origin("https://evil.com","example.com"));
}

TEST(Hub, SanitizedBroadcast) {
  int set_calls=0;
  Hub h([&](auto,auto){set_calls++;},nullptr,nullptr);
  auto c=std::make_shared<Client>(); c->room="1"; c->privileged=true;
  h.add_client(c);
  auto other=std::make_shared<Client>(); other->room="1"; other->privileged=false;
  h.add_client(other);
  h.handle_message(c, marshal_socketio("heartbeat","{\"mac_address\":\"aa\",\"student_name\":\"<b>evil</b>\"}"));
  ASSERT_FALSE(other->send_queue.empty());
  auto msg=other->send_queue.front();
  EXPECT_EQ(msg.find("<b>"), std::string::npos);
}
