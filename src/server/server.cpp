#include "server/server.hpp"
#include "session/cookie.hpp"
#include "websocket/hub.hpp"
#include <sstream>
#include <thread>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

namespace examvan::server {

static std::atomic<bool> g_running{false};
static int g_fd{-1};

Server::Server(const Config& cfg, Hub* hub, Router* router) : cfg_(cfg), hub_(hub), router_(router) {}

bool Server::has_uwebsockets() {
#ifdef HAS_UWEBSOCKETS
  return true;
#else
  return false;
#endif
}

std::string Server::describe() const {
  std::ostringstream ss;
  ss << "EXAMVAN C++ server v" << cfg_.version
     << " port=" << cfg_.port
     << " uWS=" << (has_uwebsockets() ? "yes" : "posix")
     << " routes=" << (router_ ? router_->routes().size() : 0);
  return ss.str();
}

static std::string http_response(const examvan::Response& r) {
  std::ostringstream ss;
  ss << "HTTP/1.1 " << r.status << " OK\r\n";
  for (auto& [k,v] : r.headers) ss << k << ": " << v << "\r\n";
  if (r.headers.find("Content-Length")==r.headers.end()) ss << "Content-Length: " << r.body.size() << "\r\n";
  ss << "Connection: close\r\n\r\n" << r.body;
  return ss.str();
}

static std::string ws_accept(const std::string& key) {
  std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char md[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(s.data()), s.size(), md);
  std::string raw(reinterpret_cast<char*>(md), SHA_DIGEST_LENGTH);
  int len = 4 * ((raw.size() + 2) / 3);
  std::string out(len, '\0');
  int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                          reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
  out.resize(n);
  return out;
}

static std::string extract_header(const std::string& req, const std::string& name) {
  std::string needle = name + ":";
  size_t p = req.find(needle);
  if (p == std::string::npos) {
    needle = name + " :";
    p = req.find(needle);
    if (p == std::string::npos) return "";
  }
  size_t e = req.find("\r\n", p);
  std::string line = req.substr(p + needle.size(), e - p - needle.size());
  size_t s = line.find_first_not_of(" \t");
  if (s == std::string::npos) return "";
  size_t ee = line.find_last_not_of(" \t\r\n");
  return line.substr(s, ee - s + 1);
}

static bool is_ws_upgrade(const std::string& req) {
  auto up = extract_header(req, "Upgrade");
  for (auto &c : up) c = std::tolower(c);
  return up == "websocket";
}

static void handle_ws(int cfd, const std::string& req, const std::string& path,
                      const examvan::Config& cfg, examvan::Hub* hub) {
  std::string origin = extract_header(req, "Origin");
  std::string host = extract_header(req, "Host");
  if (!examvan::check_origin(origin, host)) { close(cfd); return; }
  std::string cookie = extract_header(req, "Cookie");
  auto sess = examvan::verify_session_cookie(cfg.secret_key, cookie);
  bool privileged = sess.has_value() && sess->admin_id != 0;
  std::string key = extract_header(req, "Sec-WebSocket-Key");
  if (key.empty()) { close(cfd); return; }
  std::string accept = ws_accept(key);
  std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
  send(cfd, resp.c_str(), resp.size(), 0);
  std::string room = path.rfind("/ws/") == 0 ? path.substr(4) : path;
  auto q = room.find('?'); if (q != std::string::npos) room = room.substr(0, q);
  auto client = std::make_shared<examvan::Client>();
  client->room = room; client->privileged = privileged;
  if (hub) hub->add_client(client);
  char buf[4096];
  while (true) {
    ssize_t n = recv(cfd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    if (n < 2) continue;
    unsigned char opcode = buf[0] & 0x0F;
    bool masked = buf[1] & 0x80;
    uint64_t len = buf[1] & 0x7F;
    size_t off = 2;
    if (len == 126) { if (n < 4) break; len = (uint8_t(buf[2]) << 8) | uint8_t(buf[3]); off = 4; }
    else if (len == 127) { break; }
    unsigned char mask[4] = {0};
    if (masked) { if ((size_t)n < off + 4) break; memcpy(mask, buf + off, 4); off += 4; }
    if ((size_t)n < off + len) break;
    std::string payload(len, '\0');
    for (uint64_t i = 0; i < len; ++i) payload[i] = buf[off + i] ^ (masked ? mask[i % 4] : 0);
    if (opcode == 0x8) break;
    if (opcode == 0x9) {
      std::string pong; pong.push_back(char(0x8A)); pong.push_back(char(len));
      pong += payload; send(cfd, pong.c_str(), pong.size(), 0); continue;
    }
    if (opcode == 0x1 && hub) {
      hub->handle_message(client, payload);
      while (!client->send_queue.empty()) {
        std::string msg;
        { std::lock_guard<std::mutex> g(client->mu); if (client->send_queue.empty()) break; msg = client->send_queue.front(); client->send_queue.pop(); }
        std::string frame; frame.push_back(char(0x81));
        if (msg.size() < 126) frame.push_back(char(msg.size()));
        else if (msg.size() < 65536) { frame.push_back(char(126)); frame.push_back(char(msg.size() >> 8)); frame.push_back(char(msg.size() & 0xFF)); }
        else break;
        frame += msg;
        send(cfd, frame.c_str(), frame.size(), 0);
      }
    }
  }
  if (hub) hub->remove_client(client);
  close(cfd);
}

static void handle_client(int cfd, examvan::Router* router, const examvan::Config* cfg, examvan::Hub* hub) {
  char buf[8192]={0};
  ssize_t n=recv(cfd, buf, sizeof(buf)-1, 0);
  if(n<=0){ close(cfd); return; }
  std::string req(buf, n);
  size_t sp1=req.find(' '); size_t sp2=req.find(' ', sp1+1);
  if(sp1==std::string::npos||sp2==std::string::npos){ close(cfd); return; }
  std::string method=req.substr(0,sp1);
  std::string full_path=req.substr(sp1+1, sp2-sp1-1);
  std::string path=full_path; auto q=full_path.find('?'); if(q!=std::string::npos) path=full_path.substr(0,q);
  if (is_ws_upgrade(req) && path.rfind("/ws/",0)==0) {
    if (cfg && hub) handle_ws(cfd, req, path, *cfg, hub);
    else close(cfd);
    return;
  }
  size_t hdr_end=req.find("\r\n\r\n");
  std::string body = hdr_end!=std::string::npos ? req.substr(hdr_end+4) : "";
  std::string cookie_hdr = extract_header(req, "Cookie");
  std::string xver = extract_header(req, "X-App-Version");
  std::string origin = extract_header(req, "Origin");
  examvan::Request r;
  r.method=method; r.path=path; r.body=body;
  if(!cookie_hdr.empty()) r.headers["Cookie"]=cookie_hdr;
  if(!xver.empty()) r.headers["X-App-Version"]=xver;
  if(!origin.empty()) r.headers["Origin"]=origin;
  auto resp = router ? router->dispatch(r) : examvan::Response{};
  if(resp.status==0) resp.status=404;
  std::string out=http_response(resp);
  send(cfd, out.c_str(), out.size(), 0);
  close(cfd);
}

bool Server::listen(const ServerOpts& opts) {
  g_fd = socket(AF_INET, SOCK_STREAM, 0);
  if(g_fd<0) return false;
  int opt=1; setsockopt(g_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(opts.port);
  if(bind(g_fd,(sockaddr*)&addr,sizeof(addr))<0){ close(g_fd); g_fd=-1; return false; }
  if(::listen(g_fd, 128)<0){ close(g_fd); g_fd=-1; return false; }
  g_running=true;
  running_=true;
  std::thread([this]{
    while(g_running){
      int cfd=accept(g_fd,nullptr,nullptr);
      if(cfd<0){ if(!g_running) break; continue; }
      std::thread(handle_client, cfd, router_, &cfg_, hub_).detach();
    }
  }).detach();
  return true;
}

void Server::run() {
  while (running_ && g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

void Server::stop() {
  running_=false; g_running=false;
  if(g_fd>=0){ shutdown(g_fd, SHUT_RDWR); close(g_fd); g_fd=-1; }
}

std::string health_json(const Config& cfg) {
  return "{\"status\":\"ok\",\"version\":\"" + cfg.version + "\",\"uwebsockets\":" + (Server::has_uwebsockets() ? "true" : "false") + "}";
}

} // namespace examvan::server
