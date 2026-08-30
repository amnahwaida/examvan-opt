#include "websocket/hub.hpp"
#include "utils/sanitize.hpp"
#include "websocket/socketio.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
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
  std::vector<std::shared_ptr<Client>> snapshot;
  {
    std::lock_guard<std::mutex> g(mu_);
    auto it = rooms_.find(room_id);
    if (it==rooms_.end()) return;
    snapshot.assign(it->second.begin(), it->second.end());
  }
  std::vector<std::shared_ptr<Client>> to_remove;
  for (auto& c: snapshot) {
    if (!c->try_send(msg)) to_remove.push_back(c);
  }
  if (!to_remove.empty()) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = rooms_.find(room_id);
    if (it!=rooms_.end()) {
      for(auto& c: to_remove){ it->second.erase(c); c->close(); }
      if(it->second.empty()) rooms_.erase(it);
    }
  }
}

static std::string now_rfc3339(){
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  char buf[32];
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%SZ", &tm);
  return std::string(buf);
}

std::string Hub::extract_json_string(const std::string& json, const std::string& key){
  std::string needle = "\"" + key + "\"";
  size_t n = json.size();
  bool in_str=false; bool esc=false;
  for(size_t i=0;i<n;){
    if(!in_str && !esc && i+needle.size()<=n && json.compare(i, needle.size(), needle)==0){
      size_t colon=i+needle.size();
      while(colon<n && (json[colon]==' '||json[colon]=='\t'||json[colon]=='\n'||json[colon]=='\r')) colon++;
      if(colon<n && json[colon]==':'){
        size_t v=colon+1;
        while(v<n && (json[v]==' '||json[v]=='\t'||json[v]=='\n'||json[v]=='\r')) v++;
        if(v<n && json[v]=='"'){
          size_t q=v;
          size_t end=q+1;
          while(end<n){
            if(json[end]=='\\'){ end+=2; continue; }
            if(json[end]=='"') break;
            end++;
          }
          if(end<n) return json.substr(q+1,end-q-1);
        }
      }
    }
    char c=json[i];
    if(esc){ esc=false; }
    else if(c=='\\' && in_str){ esc=true; }
    else if(c=='"'){ in_str=!in_str; }
    i++;
  }
  return "";
}

void Hub::handle_message(std::shared_ptr<Client> c, const std::string& raw){
#ifdef HAS_PROTOBUF
  if(!raw.empty() && raw[0]!='['){
    examvan::v1::WsEnvelope env;
    if(env.ParseFromArray(raw.data(), raw.size())){
      if(env.event()=="ping"){
        examvan::v1::WsEnvelope pong;
        pong.set_event("pong");
        std::string payload = now_rfc3339();
        pong.set_payload(payload);
        std::string out; pong.SerializeToString(&out);
        c->try_send(out);
        return;
      }
      if(env.event()=="heartbeat"){ handle_heartbeat(c, env.payload()); return; }
      if(env.event()=="exam_completed"){ handle_exam_completed(c, env.payload()); return; }
      return;
    }
  }
#endif
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
  std::string mac, student_name, exam_number, student_class, device_info;
#ifdef HAS_PROTOBUF
  bool is_pb = !payload_json.empty() && payload_json[0]!='{' && payload_json[0]!='"';
  if(is_pb){
    examvan::v1::Heartbeat pb;
    if(pb.ParseFromArray(payload_json.data(), payload_json.size())){
      mac = sanitize_ws_mac(pb.mac_address());
      student_name = sanitize_ws_field(pb.student_name(),200);
      exam_number = sanitize_ws_field(pb.exam_number(),100);
      student_class = sanitize_ws_field(pb.student_class(),100);
      device_info = sanitize_ws_field(pb.device_info(),200);
    }
  }
  if(mac.empty()){
    mac = sanitize_ws_mac(extract_json_string(payload_json,"mac_address"));
    student_name = sanitize_ws_field(extract_json_string(payload_json,"student_name"),200);
    exam_number = sanitize_ws_field(extract_json_string(payload_json,"exam_number"),100);
    student_class = sanitize_ws_field(extract_json_string(payload_json,"student_class"),100);
    device_info = sanitize_ws_field(extract_json_string(payload_json,"device_info"),200);
  }
#else
  mac = sanitize_ws_mac(extract_json_string(payload_json,"mac_address"));
  student_name = sanitize_ws_field(extract_json_string(payload_json,"student_name"),200);
  exam_number = sanitize_ws_field(extract_json_string(payload_json,"exam_number"),100);
  student_class = sanitize_ws_field(extract_json_string(payload_json,"student_class"),100);
  device_info = sanitize_ws_field(extract_json_string(payload_json,"device_info"),200);
#endif
  if(mac.empty()) return;
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
  std::string mac;
#ifdef HAS_PROTOBUF
  bool is_pb = !payload_json.empty() && payload_json[0]!='{' && payload_json[0]!='"';
  if(is_pb){
    examvan::v1::ExamCompleted pb;
    if(pb.ParseFromArray(payload_json.data(), payload_json.size())) mac=sanitize_ws_mac(pb.mac_address());
  }
  if(mac.empty()) mac=sanitize_ws_mac(extract_json_string(payload_json,"mac_address"));
#else
  mac=sanitize_ws_mac(extract_json_string(payload_json,"mac_address"));
#endif
  if(mac.empty()) return;
  std::string key="heartbeat:"+std::to_string(exam_id)+":"+mac;
  if(redis_del_) redis_del_(key);
  std::string payload="{\"event\":\"exam_completed\",\"exam_id\":"+json_string(exam_id_str)+",\"mac_address\":"+json_string(mac)+"}";
  broadcast_to_room(c->room,"student_update",payload);
}

static std::string strip_port(const std::string& s){
  if(s.empty()) return s;
  if(s.front()=='['){
    auto br=s.find(']');
    if(br!=std::string::npos) return s.substr(0,br+1);
    return s;
  }
  auto c=s.rfind(':');
  if(c!=std::string::npos){
    bool has_bracket=s.find('[')!=std::string::npos;
    if(has_bracket) return s;
    std::string after=s.substr(c+1);
    bool is_port=!after.empty() && after.find_first_not_of("0123456789")==std::string::npos;
    if(is_port) return s.substr(0,c);
  }
  return s;
}
bool check_origin(const std::string& origin, const std::string& host){
  if(origin.empty()) return false;
  if(origin=="null") return false;
  std::string h = origin;
  auto p = h.find("://");
  if(p!=std::string::npos) h=h.substr(p+3);
  auto slash = h.find('/'); if(slash!=std::string::npos) h=h.substr(0,slash);
  auto colon=h.find(':'); std::string hostname=colon==std::string::npos?h:h.substr(0,colon);
  if(hostname=="localhost"||hostname=="127.0.0.1") return true;
  std::string host_no_port=host;
  auto slash2=host_no_port.find('/'); if(slash2!=std::string::npos) host_no_port=host_no_port.substr(0,slash2);
  std::string host_h=strip_port(host_no_port);
  std::string origin_h=strip_port(h);
  if(origin_h==host_h) return true;
  if(h==host) return true;
  return false;
}

}  // namespace examvan
