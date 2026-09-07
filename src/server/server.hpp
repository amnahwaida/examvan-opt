#pragma once
#include "config/config.hpp"
#include "http/router.hpp"
#include "websocket/hub.hpp"
#include <string>
#include <functional>
#include <thread>
#include <vector>

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
  /* P20-C5: worker + acceptor posix disimpan sebagai thread joinable —
   * stop() join semuanya agar graceful shutdown bisa drain (paritas jalur
   * uWS: g_uWS_thread). Tanpa ini thread hidup sampai proses mati.
   * Member TANPA guard (kosong di build uWS) — konsisten utk ODR/layout
   * di semua target (Docker HAS_UWEBSOCKETS, CI/lokal tanpa uWS). */
  std::vector<std::thread> posix_threads_;
};

std::string health_json(const Config& cfg);

} // namespace examvan::server
