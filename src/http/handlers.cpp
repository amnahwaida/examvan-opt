#include "http/handlers.hpp"
#include "session/cookie.hpp"
#include <string>
#include <fstream>
#include <sstream>

namespace examvan {

Response health_handler(const Request& req, const Config& cfg){
  (void)req;
  std::string body =
    "{\"certificate_fingerprint\":\"\","
    "\"required_app_version\":\"\","
    "\"server_time_utc\":\"\","
    "\"status\":\"healthy\","
    "\"status\":\"ok\","
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
    std::ifstream fr("templates/public/index.rendered.html");
    if(fr){
      std::ostringstream sss; sss<<fr.rdbuf();
      std::string html=sss.str();
      size_t p=html.find("2.7.3"); if(p!=std::string::npos){ html.replace(p, 5, cfg.version); }
      Response rr; rr.status=200; rr.headers["Content-Type"]="text/html"; rr.body=html; return rr;
    }
    std::ifstream f("templates/public/index.html");
    if(f){
      std::ostringstream ss; ss<<f.rdbuf();
      std::string html=ss.str();
      std::ifstream sf("templates/public/shared.html");
      if(sf){
        std::ostringstream sfs; sfs<<sf.rdbuf();
        std::string shared=sfs.str();
        auto extract=[&](const std::string& name)->std::string{
          std::string start="{{ define \""+name+"\" }}";
          std::string end="{{ end }}";
          size_t s=shared.find(start); if(s==std::string::npos) return "";
          s+=start.size(); size_t e=shared.find(end,s); if(e==std::string::npos) return "";
          return shared.substr(s, e-s);
        };
        std::string head=extract("public_head");
        std::string foot=extract("public_foot");
        size_t p;
        p=html.find("{{ template \"public_head\" . }}"); if(p!=std::string::npos) html.replace(p, 28, head);
        p=html.find("{{ template \"public_foot\" . }}"); if(p!=std::string::npos) html.replace(p, 28, foot);
        p=html.find("{{template \"public_head\" .}}"); if(p!=std::string::npos) html.replace(p, 27, head);
        p=html.find("{{template \"public_foot\" .}}"); if(p!=std::string::npos) html.replace(p, 27, foot);
      }
      auto repl=[&](const std::string& from, const std::string& to){
        size_t p=0; while((p=html.find(from,p))!=std::string::npos){ html.replace(p, from.size(), to); p+=to.size(); }
      };
      repl("{{.version}}", cfg.version);
      repl("{{ .version }}", cfg.version);
      repl("{{ version }}", cfg.version);
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
