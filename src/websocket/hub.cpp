#include "websocket/hub.hpp"
#include "utils/sanitize.hpp"
#include "websocket/socketio.hpp"
#include <chrono>
#include <ctime>
#include <sstream>

namespace examvan {

bool Client::try_send(const std::string& msg) {
  std::lock_guard<std::mutex> g(mu);
  if (closed) return false;
  if (send_queue.size() >= max_queue) return false;
  send_queue.push(msg);
  return true;
}
void Client::close() {
  std::lock_guard<std::mutex> g(mu);
  if (!closed) { closed = true; while(!send_queue.empty()) send_queue.pop(); }
}

Hub::Hub(std::function<void(const std::string&, const std::string&)> rs,
         std::function<void(const std::string&)> rd,
         std::function<void(const std::string&)> rl)
    : redis_set_(std::move(rs)), redis_del_(std::move(rd)), redis_lpush_(std::move(rl)) {}

void Hub::add_client(std::shared_ptr<Client> c) {
  std::lock_guard<std::mutex> g(mu_);
  rooms_[c->room].insert(c);
}

void Hub::remove_client(std::shared_ptr<Client> c) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = rooms_.find(c->room);
  if (it != rooms_.end()) {
    it->second.erase(c);
    c->close();
    if (it->second.empty()) rooms_.erase(it);
  }
}

size_t Hub::room_size(const std::string& room_id) const {
  std::lock_guard<std::mutex> g(mu_);
  auto it = rooms_.find(room_id);
  return it==rooms_.end()?0:it->second.size();
}

void Hub::broadcast_to_room(const std::string& room_id, const std::string& event, const std::string& payload_json) {
  std::string msg = marshal_socketio(event, payload_json);
  std::lock_guard<std::mutex> g(mu_);
  auto it = rooms_.find(room_id);
  if (it==rooms_.end()) return;
  std::vector<std::shared_ptr<Client>> to_remove;
  for (auto& c: it->second) {
    if (!c->try_send(msg)) to_remove.push_back(c);
  }
  for(auto& c: to_remove){ it->second.erase(c); c->close(); }
  if(it->second.empty()) rooms_.erase(it);
}

static std::string now_rfc3339(){
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  char buf[32]; std::strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
  return std::string(buf);
}

std::string Hub::extract_json_string(const std::string& json, const std::string& key){
  std::string needle = "\"" + key + "\"";
  auto pos = json.find(needle);
  if(pos==std::string::npos) return "";
  pos = json.find(':',pos);
  if(pos==std::string::npos) return "";
  pos = json.find('"',pos);
  if(pos==std::string::npos) return "";
  auto end = json.find('"',pos+1);
  if(end==std::string::npos) return "";
  return json.substr(pos+1,end-pos-1);
}

void Hub::handle_message(std::shared_ptr<Client> c, const std::string& raw){
  auto msg = parse_socketio(raw);
  if(!msg) return;
  if(msg->event=="ping"){
    std::string payload = json_string(now_rfc3339());
    c->try_send(marshal_socketio("pong", payload));
    return;
  }
  if(msg->event=="heartbeat"){ handle_heartbeat(c, msg->payload_json); return; }
  if(msg->event=="exam_completed"){ handle_exam_completed(c, msg->payload_json); return; }
}

void Hub::handle_heartbeat(std::shared_ptr<Client> c, const std::string& payload_json){
  if(!c->privileged) return;
  std::string exam_id_str = c->room;
  int exam_id=0; try{exam_id=std::stoi(exam_id_str);}catch(...){return;}
  if(exam_id==0) return;
  std::string mac = sanitize_ws_mac(extract_json_string(payload_json,"mac_address"));
  if(mac.empty()) return;
  std::string student_name = sanitize_ws_field(extract_json_string(payload_json,"student_name"),200);
  std::string exam_number = sanitize_ws_field(extract_json_string(payload_json,"exam_number"),100);
  std::string student_class = sanitize_ws_field(extract_json_string(payload_json,"student_class"),100);
  std::string device_info = sanitize_ws_field(extract_json_string(payload_json,"device_info"),200);
  std::string last_seen = now_rfc3339();

  std::string key = "heartbeat:" + std::to_string(exam_id) + ":" + mac;
  std::string hb_json = "{\"student_name\":"+json_string(student_name)+
    ",\"exam_number\":"+json_string(exam_number)+
    ",\"student_class\":"+json_string(student_class)+
    ",\"device_info\":"+json_string(device_info)+
    ",\"event\":\"heartbeat\",\"last_seen\":"+json_string(last_seen)+"}";
  if(redis_set_) redis_set_(key, hb_json);
  if(redis_lpush_){
    std::string qp="{\"exam_id\":"+std::to_string(exam_id)+",\"mac_address\":"+json_string(mac)+
      ",\"student_name\":"+json_string(student_name)+",\"exam_number\":"+json_string(exam_number)+
      ",\"student_class\":"+json_string(student_class)+",\"event\":\"heartbeat\",\"last_seen\":"+json_string(last_seen)+"}";
    redis_lpush_(qp);
  }
  std::string broadcast_payload="{\"student_name\":"+json_string(student_name)+
    ",\"exam_number\":"+json_string(exam_number)+
    ",\"student_class\":"+json_string(student_class)+
    ",\"event\":\"heartbeat\",\"last_seen\":"+json_string(last_seen)+
    ",\"exam_id\":"+json_string(exam_id_str)+",\"mac_address\":"+json_string(mac)+"}";
  broadcast_to_room(c->room,"student_update",broadcast_payload);
}

void Hub::handle_exam_completed(std::shared_ptr<Client> c, const std::string& payload_json){
  if(!c->privileged) return;
  std::string exam_id_str=c->room; int exam_id=0; try{exam_id=std::stoi(exam_id_str);}catch(...){return;} if(exam_id==0) return;
  std::string mac=sanitize_ws_mac(extract_json_string(payload_json,"mac_address"));
  if(mac.empty()) return;
  std::string key="heartbeat:"+std::to_string(exam_id)+":"+mac;
  if(redis_del_) redis_del_(key);
  std::string payload="{\"event\":\"exam_completed\",\"exam_id\":"+json_string(exam_id_str)+",\"mac_address\":"+json_string(mac)+"}";
  broadcast_to_room(c->room,"student_update",payload);
}

bool check_origin(const std::string& origin, const std::string& host){
  if(origin.empty()) return true;
  std::string h = origin;
  auto p = h.find("://");
  if(p!=std::string::npos) h=h.substr(p+3);
  auto slash = h.find('/'); if(slash!=std::string::npos) h=h.substr(0,slash);
  if(h==host) return true;
  auto colon=h.find(':'); std::string hostname=colon==std::string::npos?h:h.substr(0,colon);
  return hostname=="localhost"||hostname=="127.0.0.1";
}

}  // namespace examvan
