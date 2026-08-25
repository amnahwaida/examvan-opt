#include <gtest/gtest.h>
#include "server/server.hpp"
#include "http/router_full.hpp"
#include "websocket/hub.hpp"
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

using namespace examvan;

static std::string http_get(const std::string& host, int port, const std::string& path){
  int fd=socket(AF_INET,SOCK_STREAM,0);
  sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(port);
  inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  if(connect(fd,(sockaddr*)&addr,sizeof(addr))<0){ close(fd); return ""; }
  std::string req="GET "+path+" HTTP/1.1\r\nHost: "+host+"\r\nConnection: close\r\n\r\n";
  send(fd, req.c_str(), req.size(), 0);
  char buf[8192]={0}; recv(fd, buf, sizeof(buf)-1, 0);
  close(fd);
  return std::string(buf);
}

TEST(ServerLive, HealthAndRouting){
  Config cfg; cfg.port=18080; cfg.version="2.7.2";
  cfg.secret_key="test-secret-1234567890abcdef12345678";
  cfg.admin_user="superadmin"; cfg.admin_pass="pass";
  cfg.r2_access_key="k"; cfg.r2_secret_key="s"; cfg.r2_endpoint="https://ep";
  Hub hub; Router router; register_full_routes(router,cfg);
  server::Server srv(cfg,&hub,&router);
  ASSERT_TRUE(srv.listen({18080}));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auto resp=http_get("127.0.0.1",18080,"/api/health");
  EXPECT_NE(resp.find("200 OK"), std::string::npos);
  EXPECT_NE(resp.find("\"status\":\"ok\""), std::string::npos);
  auto resp2=http_get("127.0.0.1",18080,"/hasil");
  EXPECT_NE(resp2.find("Cek Hasil"), std::string::npos);
  srv.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
