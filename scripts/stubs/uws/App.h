// Minimal stub of uWebSockets App.h — syntax-check only, NOT a real
// implementation. Dipakai scripts/check-docker-paths.sh untuk men-compile
// jalur #ifdef HAS_UWEBSOCKETS di mesin dev tanpa uWS. Cakupan: hanya API
// yang dipakai src/server/server.cpp (App::get/any/ws/listen/run/close,
// HttpResponse, HttpRequest, WebSocket, WebSocketBehavior, OpCode).
#pragma once
#include <string>
#include <string_view>
#include <functional>
#include <utility>

namespace uWS {

enum class OpCode { CONTINUATION, TEXT, BINARY, CLOSE, PING, PONG };

template <bool SSL>
struct HttpRequest {
  std::string_view data;
  std::string_view getUrl() const { return data; }
  std::string_view getMethod() const { return data; }
  std::string_view getHeader(std::string_view) const { return data; }
  std::string_view getParameter(int) const { (void)0; return data; }
};

template <bool SSL, class UserData>
struct WebSocket {
  UserData* user_data = nullptr;
  UserData* getUserData() { return user_data; }
  unsigned int getBufferedAmount() { return 0; }
  bool send(std::string_view, OpCode) { return true; }
  void close() {}
};

template <bool SSL>
struct HttpResponse {
  bool responded = false;
  HttpResponse& writeStatus(const std::string&) { return *this; }
  HttpResponse& writeHeader(const std::string&, const std::string&) { return *this; }
  void end() {}
  void end(const std::string&) {}
  void end(const std::string&, bool) {}
  void onAborted(std::function<void()>) {}
  void onData(std::function<void(std::string_view, bool)>) {}
  bool hasResponded() { return responded; }
  void close() {}
  template <class UserData>
  void upgrade(UserData&&, std::string_view, std::string_view, std::string_view, void*) {}
};

template <class UserData>
struct WebSocketBehavior {
  unsigned int maxPayloadLength = 0;
  unsigned int idleTimeout = 0;
  unsigned int maxBackpressure = 0;
  std::function<void(HttpResponse<false>*, HttpRequest<false>*, void*)> upgrade;
  std::function<void(WebSocket<false, UserData>*)> open;
  std::function<void(WebSocket<false, UserData>*, std::string_view, OpCode)> message;
  std::function<void(WebSocket<false, UserData>*, int, std::string_view)> close;
};

struct App {
  template <class Handler> App& get(const std::string&, Handler&&) { return *this; }
  template <class Handler> App& any(const std::string&, Handler&&) { return *this; }
  template <class UserData>
  App& ws(const std::string&, WebSocketBehavior<UserData>&&) { return *this; }
  template <class Port, class Handler>
  App& listen(Port&&, Handler&&) { return *this; }
  void run() {}
  void close() {}
};

} // namespace uWS
