#pragma once
#include <string>
#include <map>
#include <functional>
#include <vector>

namespace examvan {

struct Request {
  std::string method;
  std::string path;
  std::map<std::string,std::string> headers;
  std::string body;
  std::string query;
  std::map<std::string,std::string> params;
  std::map<std::string,std::string> cookies() const;
};

struct Response {
  int status{200};
  std::map<std::string,std::string> headers;
  std::string body;
  void json(int code, const std::string& j);
  void text(int code, const std::string& t);
};

using Handler = std::function<Response(const Request&)>;

class Router {
 public:
  void add(const std::string& method, const std::string& path, Handler h);
  Response dispatch(const Request& req) const;
  std::vector<std::string> routes() const;
 private:
  struct Route { std::string method, path; Handler handler; };
  std::vector<Route> routes_;
  static bool match(const std::string& pattern, const std::string& path, std::map<std::string,std::string>& out);
};

}  // namespace examvan
