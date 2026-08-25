#include "http/handlers.hpp"
#include <string>

namespace examvan {

Response health_handler(const Request& req, const Config& cfg){
  (void)req;
  std::string body="{\"status\":\"ok\",\"version\":\""+cfg.version+"\"}";
  Response res; res.json(200,body); return res;
}

void register_routes(Router& r, const Config& cfg){
  r.add("GET","/api/health", [&cfg](const Request& req){ return health_handler(req,cfg); });
  r.add("GET","/health", [&cfg](const Request& req){ return health_handler(req,cfg); });
  r.add("GET","/", [](const Request&){ Response res; res.text(200,"EXAMVAN C++ 2.7.2"); return res; });
  r.add("GET","/ws/:room_id", [](const Request& req){
    auto it=req.params.find("room_id");
    std::string room=it!=req.params.end()?it->second:"";
    Response res; res.json(101,"{\"upgrade\":\"websocket\",\"room\":\""+room+"\"}"); return res;
  });
}

}  // namespace examvan
