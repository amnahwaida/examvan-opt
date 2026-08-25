#pragma once
#include "config/config.hpp"
#include "http/router.hpp"
#include "websocket/hub.hpp"
#include <string>
#include <functional>

namespace examvan::server {

struct ServerOpts {
  int port{5000};
  std::string host{"0.0.0.0"};
};

class Server {
public:
  explicit Server(const Config& cfg, Hub* hub, Router* router);
  bool listen(const ServerOpts& opts);
  void run();
  void stop();
  static bool has_uwebsockets();
  std::string describe() const;
private:
  const Config& cfg_;
  Hub* hub_;
  Router* router_;
  bool running_{false};
};

std::string health_json(const Config& cfg);

} // namespace examvan::server
