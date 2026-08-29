#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace examvan {

struct Client {
  std::string id;
  std::string room;
  bool privileged{false};
  bool closed{false};
  std::queue<std::string> send_queue;
  static constexpr size_t max_queue = 256;
  std::mutex mu;
  bool try_send(const std::string& msg);
  void close();
};

struct HeartbeatData {
  std::string student_name;
  std::string exam_number;
  std::string student_class;
  std::string device_info;
  std::string mac_address;
  int exam_id{0};
};

class Hub {
 public:
  explicit Hub(std::function<void(const std::string&, const std::string&)> redis_set = nullptr,
               std::function<void(const std::string&)> redis_del = nullptr,
               std::function<void(const std::string&)> redis_lpush = nullptr);

  void add_client(std::shared_ptr<Client> c);
  void remove_client(std::shared_ptr<Client> c);
  void broadcast_to_room(const std::string& room_id, const std::string& event, const std::string& payload_json);
  size_t room_size(const std::string& room_id) const;
  void handle_message(std::shared_ptr<Client> c, const std::string& raw);

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unordered_set<std::shared_ptr<Client>>> rooms_;
  std::function<void(const std::string&, const std::string&)> redis_set_;
  std::function<void(const std::string&)> redis_del_;
  std::function<void(const std::string&)> redis_lpush_;

  void handle_heartbeat(std::shared_ptr<Client> c, const std::string& payload_json);
  void handle_exam_completed(std::shared_ptr<Client> c, const std::string& payload_json);
  static bool is_valid_origin(const std::string& origin, const std::string& host);
 public:
  static std::string extract_json_string(const std::string& json, const std::string& key);
 private:
};

bool check_origin(const std::string& origin, const std::string& host);

}  // namespace examvan
