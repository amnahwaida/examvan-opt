#include "http/handlers.hpp"
#include "session/cookie.hpp"
#include <string>
#include <fstream>
#include <sstream>

namespace examvan {

Response health_handler(const Request& req, const Config& cfg){
  (void)req;
  /* Paritas Go health.go (json-schema, 6 key persis) — sama dengan
   * handlers::api::health; register_routes dasar harus identik router_full. */
  std::string body =
    "{\"certificate_fingerprint\":\"\","
    "\"required_app_version\":\"\","
    "\"server_time_utc\":\"\","
    "\"status\":\"healthy\","
    "\"success\":true,"
    "\"version\":\""+cfg.version+"\"}";
  Response res; res.json(200,body); return res;
}

void register_routes(Router& r, const Config& cfg){
  r.add("GET","/api/health", [&cfg](const Request& req){ return health_handler(req,cfg); });
  r.add("GET","/health", [&cfg](const Request& req){ return health_handler(req,cfg); });
  r.add("GET","/", [cfg](const Request& req) mutable {
    auto it=req.headers.find("Cookie");
    if(it!=req.headers.end()){
      auto s=verify_session_cookie(cfg.secret_key, it->second);
      if(s.has_value() && s->admin_id!=0){
        Response rr; rr.status=302; rr.headers["Location"]="/admin/dashboard"; return rr;
      }
    }
    std::ifstream f("templates/public/index.html");
    if(f){
      std::ostringstream ss; ss<<f.rdbuf();
      std::string html=ss.str();
      size_t p=html.find("{{ version }}"); if(p!=std::string::npos) html.replace(p, 13, cfg.version);
      p=html.find("{{version}}"); if(p!=std::string::npos) html.replace(p, 11, cfg.version);
      Response rr; rr.status=200; rr.headers["Content-Type"]="text/html"; rr.body=html; return rr;
    }
    Response rr; rr.status=200; rr.headers["Content-Type"]="text/html";
    rr.body="<html><head><title>EXAMVAN "+cfg.version+"</title></head><body><nav><a href=\"/login\">Login</a> <a href=\"/hasil\">Cek Hasil</a> <a href=\"/download\">Download</a></nav><h1>EXAMVAN Platform Ujian</h1><p>Versi "+cfg.version+"</p></body></html>";
    return rr;
  });
  r.add("GET","/ws/:room_id", [](const Request& req){
    auto it=req.params.find("room_id");
    std::string room=it!=req.params.end()?it->second:"";
    Response res; res.json(101,"{\"upgrade\":\"websocket\",\"room\":\""+room+"\"}"); return res;
  });
}

}  // namespace examvan
