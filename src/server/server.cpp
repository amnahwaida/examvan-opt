#include "server/server.hpp"
#include <sstream>
#include <thread>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

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

static void handle_client(int cfd, examvan::Router* router) {
  char buf[8192]={0};
  ssize_t n=recv(cfd, buf, sizeof(buf)-1, 0);
  if(n<=0){ close(cfd); return; }
  std::string req(buf, n);
  size_t sp1=req.find(' '); size_t sp2=req.find(' ', sp1+1);
  if(sp1==std::string::npos||sp2==std::string::npos){ close(cfd); return; }
  std::string method=req.substr(0,sp1);
  std::string full_path=req.substr(sp1+1, sp2-sp1-1);
  std::string path=full_path; auto q=full_path.find('?'); if(q!=std::string::npos) path=full_path.substr(0,q);
  size_t hdr_end=req.find("\r\n\r\n");
  std::string body = hdr_end!=std::string::npos ? req.substr(hdr_end+4) : "";
  std::string cookie_hdr;
  size_t ck=req.find("Cookie:"); if(ck!=std::string::npos){ size_t e=req.find("\r\n",ck); cookie_hdr=req.substr(ck+7, e-ck-7); }
  std::string xver;
  size_t xv=req.find("X-App-Version:"); if(xv!=std::string::npos){ size_t e=req.find("\r\n",xv); xver=req.substr(xv+16, e-xv-16); xver.erase(0,xver.find_first_not_of(" \t")); }
  examvan::Request r;
  r.method=method; r.path=path; r.body=body;
  if(!cookie_hdr.empty()) r.headers["Cookie"]=cookie_hdr;
  if(!xver.empty()) r.headers["X-App-Version"]=xver;
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
      std::thread(handle_client, cfd, router_).detach();
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
