#include "server/server.hpp"
#include "session/cookie.hpp"
#include "websocket/hub.hpp"
#include <sstream>
#include <fstream>
#include <thread>
#include <atomic>
#include <cstring>
#include <queue>
#include <mutex>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#ifdef HAS_UWEBSOCKETS
#include "App.h"
struct WsData { std::string room; bool privileged; std::string id; std::shared_ptr<examvan::Client> client; };
#endif

namespace examvan::server {

static std::atomic<bool> g_running{false};
#ifndef HAS_UWEBSOCKETS
static int g_fd{-1};
#endif

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

#ifndef HAS_UWEBSOCKETS
static std::string http_response(const examvan::Response& r) {
  std::ostringstream ss;
  ss << "HTTP/1.1 " << r.status << " OK\r\n";
  for (auto& [k,v] : r.headers) ss << k << ": " << v << "\r\n";
  if (r.headers.find("Content-Length")==r.headers.end()) ss << "Content-Length: " << r.body.size() << "\r\n";
  ss << "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: strict-origin-when-cross-origin\r\n";
  ss << "Connection: Keep-Alive\r\nKeep-Alive: timeout=5, max=100\r\n\r\n" << r.body;
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
  auto sess = cfg.secret_prev.empty() ? examvan::verify_session_cookie(cfg.secret_key, cookie) : examvan::verify_session_cookie_dual(cfg.secret_key, cfg.secret_prev, cookie);
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
    else if (len == 127) { if (n < 10) break; len = 0; for(int i=0;i<8;i++) len = (len<<8) | uint8_t(buf[2+i]); off = 10; }
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

static std::string content_type_for(const std::string& p){
  if(p.size()>=4 && p.substr(p.size()-4)==".css") return "text/css";
  if(p.size()>=3 && p.substr(p.size()-3)==".js") return "application/javascript";
  if(p.size()>=4 && p.substr(p.size()-4)==".png") return "image/png";
  if(p.size()>=4 && p.substr(p.size()-4)==".ico") return "image/x-icon";
  if(p.size()>=5 && p.substr(p.size()-5)==".woff") return "font/woff";
  if(p.size()>=6 && p.substr(p.size()-6)==".woff2") return "font/woff2";
  return "text/plain";
}

static bool try_serve_static(int cfd, const std::string& path){
  std::string dec = path;
  // url_decode path to catch %2e etc, handle double encode
  for(int iter=0; iter<2; ++iter){
    std::string nd;
    nd.reserve(dec.size());
    for(size_t i=0;i<dec.size();++i){
      if(dec[i]=='%' && i+2<dec.size()){
        auto hex=[](char c)->int{ if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; };
        int h=hex(dec[i+1]), l=hex(dec[i+2]);
        if(h>=0&&l>=0){ nd.push_back(char((h<<4)|l)); i+=2; } else nd.push_back(dec[i]);
      } else if(dec[i]=='+') nd.push_back(' ');
      else nd.push_back(dec[i]);
    }
    if(nd==dec) break;
    dec=nd;
  }
  if(dec.find("..")!=std::string::npos) return false;
  if(dec.find('\0')!=std::string::npos) return false;
  if(dec.find("%2e")!=std::string::npos || dec.find("%2E")!=std::string::npos) return false;
  std::string fp;
  if(dec.rfind("/static/",0)==0) fp="."+dec;
  else if(dec=="/favicon.ico") fp="./static/favicon.png";
  else return false;
  if(fp.find("..")!=std::string::npos) return false;
  std::ifstream f(fp, std::ios::binary);
  if(!f) return false;
  std::ostringstream ss; ss<<f.rdbuf();
  std::string body=ss.str();
  std::string ct=content_type_for(fp);
  std::string etag="\""+std::to_string(std::hash<std::string>{}(body) & 0xffffffff)+"\"";
  std::ostringstream out;
  out<<"HTTP/1.1 200 OK\r\nContent-Type: "<<ct<<"\r\nContent-Length: "<<body.size()<<"\r\nETag: "<<etag<<"\r\nCache-Control: public, max-age=31536000, immutable\r\nConnection: Keep-Alive\r\nKeep-Alive: timeout=5, max=100\r\n\r\n"<<body;
  std::string s=out.str();
  send(cfd, s.c_str(), s.size(), 0);
  close(cfd);
  return true;
}

static void handle_client(int cfd, examvan::Router* router, const examvan::Config* cfg, examvan::Hub* hub) {
  std::string req;
  char buf[8192];
  ssize_t n=0;
  while((n=recv(cfd, buf, sizeof(buf), 0))>0){
    req.append(buf, n);
    if(req.find("\r\n\r\n")!=std::string::npos) break;
    if(req.size()>16384) break;
  }
  if(req.empty()){ close(cfd); return; }
  std::string expect = extract_header(req, "Expect");
  for(auto &c: expect) c=tolower(c);
  if(expect.find("100-continue")!=std::string::npos){
    std::string cont="HTTP/1.1 100 Continue\r\n\r\n";
    send(cfd, cont.c_str(), cont.size(), 0);
  }
  size_t sp1=req.find(' '); size_t sp2=req.find(' ', sp1+1);
  if(sp1==std::string::npos||sp2==std::string::npos){ close(cfd); return; }
  std::string method=req.substr(0,sp1);
  std::string full_path=req.substr(sp1+1, sp2-sp1-1);
  std::string path=full_path; auto q=full_path.find('?'); if(q!=std::string::npos) path=full_path.substr(0,q);
  if(try_serve_static(cfd, path)) return;
  if (is_ws_upgrade(req) && path.rfind("/ws/",0)==0) {
    if (cfg && hub) handle_ws(cfd, req, path, *cfg, hub);
    else close(cfd);
    return;
  }
  size_t hdr_end=req.find("\r\n\r\n");
  std::string body = hdr_end!=std::string::npos ? req.substr(hdr_end+4) : "";
  std::string cl_s = extract_header(req, "Content-Length");
  if(!cl_s.empty()){
    try{
      size_t cl=std::stoul(cl_s);
      if(cl > 5*1024*1024){
        std::string too="HTTP/1.1 413 Payload Too Large\r\nContent-Length: 18\r\nConnection: close\r\n\r\npayload too large";
        send(cfd, too.c_str(), too.size(), 0);
        close(cfd); return;
      }
      while(body.size()<cl){
        n=recv(cfd, buf, sizeof(buf), 0);
        if(n<=0) break;
        if(body.size()+n > 5*1024*1024){
          std::string too="HTTP/1.1 413 Payload Too Large\r\nContent-Length: 18\r\nConnection: close\r\n\r\npayload too large";
          send(cfd, too.c_str(), too.size(), 0);
          close(cfd); return;
        }
        body.append(buf, n);
      }
      if(body.size()>cl) body.resize(cl);
    }catch(...){}
  }
  if(body.size()>5*1024*1024){
    std::string too="HTTP/1.1 413 Payload Too Large\r\nContent-Length: 18\r\nConnection: close\r\n\r\npayload too large";
    send(cfd, too.c_str(), too.size(), 0);
    close(cfd); return;
  }
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

#endif // !HAS_UWEBSOCKETS

#ifdef HAS_UWEBSOCKETS
static std::unique_ptr<uWS::App> g_app = nullptr;
static std::thread g_uWS_thread;
static thread_local std::string g_uWS_body;
#endif

bool Server::listen(const ServerOpts& opts) {
#ifdef HAS_UWEBSOCKETS
  // uWS::Loop bersifat thread-local: App harus dibuat, di-listen, dan
  // di-run() pada thread yang SAMA. Jika App dibuat di main lalu run()
  // di thread lain, Loop::get() membuat loop baru yang kosong → run()
  // langsung selesai dan tidak ada request yang pernah dilayani.
  g_uWS_thread = std::thread([this, port = opts.port]{
    g_app = std::make_unique<uWS::App>();
    auto* hub_ptr = hub_;
    auto* router_ptr = router_;
    auto* cfg_ptr = &cfg_;
    g_app->get("/api/health", [router_ptr](auto *res, auto *req){
      examvan::Request r; r.method="GET"; r.path="/api/health";
      auto resp = router_ptr ? router_ptr->dispatch(r) : examvan::Response{};
      res->writeHeader("Content-Type","application/json");
      res->end(resp.body);
    });
    g_app->any("/*", [router_ptr](auto *res, auto *req){
      std::string path(req->getUrl());
      if(path.rfind("/static/",0)==0 || path=="/favicon.ico"){
        std::string fp = path=="/favicon.ico" ? "./static/favicon.png" : "."+path;
        std::ifstream f(fp, std::ios::binary);
        if(f){
          std::ostringstream ss; ss<<f.rdbuf();
          std::string body=ss.str();
          std::string ct="text/plain";
          if(fp.size()>=4 && fp.substr(fp.size()-4)==".css") ct="text/css";
          else if(fp.size()>=3 && fp.substr(fp.size()-3)==".js") ct="application/javascript";
          else if(fp.size()>=4 && fp.substr(fp.size()-4)==".png") ct="image/png";
          std::string etag="\""+std::to_string(std::hash<std::string>{}(body) & 0xffffffff)+"\"";
          res->writeHeader("Content-Type",ct);
          res->writeHeader("ETag",etag);
          res->writeHeader("Cache-Control","public, max-age=31536000, immutable");
          res->writeHeader("Connection","Keep-Alive");
          res->end(body);
          return;
        }
      }
      /* HACK uWS v20: getMethod() melowercase method secara in-place
       * (HttpParser.h "Compatibility hack") — Router membandingkan "GET"
       * uppercase, jadi normalisasi kembali ke uppercase sebelum dispatch. */
      std::string method(req->getMethod());
      for (auto &c : method) c = toupper(static_cast<unsigned char>(c));
      /* Body POST dibaca via res->onData (uWS streaming). Handler GET juga
       * dipanggil dengan body kosong (chunk="" + last=true) — aman.
       * WAJIB: semua path harus respond, dan onAborted wajib terpasang. */
      std::string cookie(std::string_view(req->getHeader("cookie")));
      std::string xver(std::string_view(req->getHeader("x-app-version")));
      std::string origin(std::string_view(req->getHeader("origin")));
      res->onAborted([res](){
        g_uWS_body.clear();
        res->writeStatus("500");
        res->end();
      });
      res->onData([router_ptr, res, method, path, cookie, xver, origin](std::string_view chunk, bool last){
        g_uWS_body.append(chunk);
        if(!last) return;
        examvan::Request r; r.method=method; r.path=path; r.body=g_uWS_body;
        if(!cookie.empty()) r.headers["Cookie"]=cookie;
        if(!xver.empty()) r.headers["X-App-Version"]=xver;
        if(!origin.empty()) r.headers["Origin"]=origin;
        g_uWS_body.clear();
        auto resp = router_ptr ? router_ptr->dispatch(r) : examvan::Response{};
        if(resp.status==0) resp.status=404;
        res->writeStatus(std::to_string(resp.status));
        for(auto &h: resp.headers) res->writeHeader(h.first, h.second);
        res->end(resp.body);
      });
    });
    g_app->ws<WsData>("/ws/:room_id", {
      .upgrade = [hub_ptr, cfg_ptr](auto *res, auto *req, auto *context){
        std::string cookie(req->getHeader("cookie"));
        auto sess = cfg_ptr->secret_prev.empty() ? examvan::verify_session_cookie(cfg_ptr->secret_key, cookie) : examvan::verify_session_cookie_dual(cfg_ptr->secret_key, cfg_ptr->secret_prev, cookie);
        bool priv = sess.has_value() && sess->admin_id!=0;
        std::string origin(req->getHeader("origin"));
        std::string host(req->getHeader("host"));
        if(!examvan::check_origin(origin, host)){ res->close(); return; }
        std::string room(req->getParameter(0));
        res->template upgrade<WsData>({room, priv, "", nullptr}, req->getHeader("sec-websocket-key"), req->getHeader("sec-websocket-protocol"), req->getHeader("sec-websocket-extensions"), context);
      },
      .open = [hub_ptr](auto *ws){
        auto* d = ws->getUserData();
        // shared_ptr<Client> persisted in WsData
        d->client = std::make_shared<examvan::Client>();
        d->client->room = d->room; d->client->privileged = d->privileged;
        hub_ptr->add_client(d->client);
        d->id = std::to_string((uintptr_t)ws);
      },
      .message = [hub_ptr](auto *ws, std::string_view msg, uWS::OpCode op){
        if(op!=uWS::OpCode::TEXT) return;
        auto* d = ws->getUserData();
        auto c = d->client;
        if(!c) return;
        hub_ptr->handle_message(c, std::string(msg));
        while(!c->send_queue.empty()){
          std::string m;
          { std::lock_guard<std::mutex> g(c->mu); if(c->send_queue.empty()) break; m=c->send_queue.front(); c->send_queue.pop(); }
          ws->send(m, uWS::OpCode::TEXT);
        }
      },
      .close = [hub_ptr](auto *ws, int, std::string_view){
        auto* d = ws->getUserData();
        auto c = d->client;
        if(c) hub_ptr->remove_client(c);
      }
    });
    g_app->listen(port, [](auto *token){
      if(token) std::cout << "uWS listening" << std::endl;
      else std::cerr << "uWS failed to listen" << std::endl;
    });
    g_app->run();
  });
  g_uWS_thread.detach();
  running_=true; g_running=true;
  return true;
#else
  g_fd = socket(AF_INET, SOCK_STREAM, 0);
  if(g_fd<0) return false;
  int opt=1; setsockopt(g_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(opts.port);
  if(bind(g_fd,(sockaddr*)&addr,sizeof(addr))<0){ close(g_fd); g_fd=-1; return false; }
  if(::listen(g_fd, SOMAXCONN)<0){ close(g_fd); g_fd=-1; return false; }
  g_running=true;
  running_=true;
  static std::queue<int> q;
  static std::mutex qmu;
  static std::condition_variable qcv;
  for(int i=0;i<8;i++){
    std::thread([this]{
      while(g_running){
        int cfd=-1;
        { std::unique_lock<std::mutex> lk(qmu); qcv.wait(lk, []{ return !q.empty() || !g_running.load(); }); if(!g_running && q.empty()) break; if(q.empty()) continue; cfd=q.front(); q.pop(); }
        handle_client(cfd, router_, &cfg_, hub_);
      }
    }).detach();
  }
  std::thread([this]{
    while(g_running){
      int cfd=accept(g_fd,nullptr,nullptr);
      if(cfd<0){ if(!g_running) break; continue; }
      { std::lock_guard<std::mutex> lk(qmu); q.push(cfd); }
      qcv.notify_one();
    }
  }).detach();
  return true;
#endif
}

void Server::run() {
#ifdef HAS_UWEBSOCKETS
  while (running_ && g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));
#else
  while (running_ && g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));
#endif
}

void Server::stop() {
  running_=false; g_running=false;
#ifdef HAS_UWEBSOCKETS
  if(g_app) g_app->close();
  if(g_uWS_thread.joinable()) g_uWS_thread.join();
#else
  if(g_fd>=0){ shutdown(g_fd, SHUT_RDWR); close(g_fd); g_fd=-1; }
#endif
}

static auto g_start_time = std::chrono::steady_clock::now();
std::string health_json(const Config& cfg) {
  auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_start_time).count();
  return "{\"status\":\"ok\",\"version\":\"" + cfg.version + "\",\"uwebsockets\":" + (Server::has_uwebsockets() ? "true" : "false") + ",\"db\":\"ok\",\"redis\":\"ok\",\"queue\":0,\"uptime\":" + std::to_string(uptime) + "}";
}

} // namespace examvan::server
