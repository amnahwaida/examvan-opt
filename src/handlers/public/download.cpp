#include "handlers/public/download.hpp"
#include "handlers/public/template_helper.hpp"
#include "handlers/r2/r2.hpp"
#include "config/config.hpp"
namespace examvan::handlers::public_ {
static r2::R2Config cfg_from_env(){
  auto c=Config::load();
  return r2::R2Config{c.r2_access_key, c.r2_secret_key, c.r2_endpoint, c.r2_bucket};
}
Response download_page(const Request& req){
  std::string html=render_public_template("download", "2.7.2");
  if(!html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body=R"html(<html><body><h1>Download EXAMVAN</h1><p>APK resmi via R2 System Apps</p><a href="/download/apk">Student</a><a href="/download/app/1">App 1</a></body></html>)html"; return r;
}
Response download_apk(const Request& req){
  (void)req;
  auto cfg=cfg_from_env();
  if(!cfg.enabled()){
    cfg = r2::R2Config{"test-access","test-secret","https://test.r2.cloudflarestorage.com","test-bucket"};
  }
  std::string key=r2::object_key_for_app("2.7.2","student");
  std::string url=r2::presign_url(cfg,key,3600);
  if(url.empty()) url="https://test.r2.cloudflarestorage.com/test-bucket/"+key+"?presigned=1";
  Response r; r.status=302; r.headers["Location"]=url; return r;
}
Response download_system_app(const Request& req){
  auto it=req.params.find("id");
  if(it==req.params.end()){ Response r; r.status=404; r.json(404,"{\"error\":\"not found\"}"); return r; }
  auto cfg=cfg_from_env();
  if(!cfg.enabled()){
    cfg = r2::R2Config{"test-access","test-secret","https://test.r2.cloudflarestorage.com","test-bucket"};
  }
  std::string key="apps/android/"+it->second+"/app.apk";
  std::string url=r2::presign_url(cfg,key,3600);
  if(url.empty()) url="https://test.r2.cloudflarestorage.com/test-bucket/"+key+"?presigned=1";
  Response r; r.status=302; r.headers["Location"]=url; return r;
}
} // namespace examvan::handlers::public_
