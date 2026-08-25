#include <gtest/gtest.h>
#include "websocket/socketio.hpp"
using namespace examvan;

TEST(SocketIO, MarshalParse) {
  std::string payload = "{\"x\":1}";
  auto raw = marshal_socketio("ping", payload);
  auto parsed = parse_socketio(raw);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->event, "ping");
  EXPECT_EQ(parsed->payload_json, payload);
}

TEST(SocketIO, ParseInvalid) {
  EXPECT_FALSE(parse_socketio("not json").has_value());
  EXPECT_FALSE(parse_socketio("[\"only_one\"]").has_value());
}

TEST(SocketIO, MakePayload) {
  auto j = make_payload({{"a","1"},{"b","2"}});
  EXPECT_NE(j.find("\"a\""), std::string::npos);
  EXPECT_NE(j.find("\"b\""), std::string::npos);
}

TEST(SocketIO, PongRoundTrip) {
  auto raw = marshal_socketio("pong", "\"2026-01-01T00:00:00Z\"");
  auto p = parse_socketio(raw);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->event, "pong");
}
