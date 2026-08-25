#pragma once
#include <optional>
#include <string>
#include <map>

namespace examvan {

struct SocketIOMessage {
  std::string event;
  std::string payload_json;
};

std::string marshal_socketio(const std::string& event, const std::string& payload_json);
std::optional<SocketIOMessage> parse_socketio(const std::string& data);
std::string json_string(const std::string& s);
std::string make_payload(const std::map<std::string,std::string>& kv);

}  // namespace examvan
