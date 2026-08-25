#include "handlers/public/download.hpp"
#include "handlers/r2/r2.hpp"
namespace examvan::handlers::public_ {
Response download_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body=R"html(<html><body><h1>Download EXAMVAN</h1><p>APK resmi via R2 System Apps</p><a href="/download/apk">Student</a><a href="/download/app/1">App 1</a></body></html>)html"; return r;
}
Response download_apk(const Request& req){
  (void)req;
  r2::R2Config cfg{"938c2a78e0f549419cc797d154904939","4ab5063738942a24451dbb0da44a110047eacdbf95eafbf19c92479d35e0dd3c","https://c5a009838cc2bfc1bec1ae19c17f28ff.r2.cloudflarestorage.com","examvan-bucket"};
  if(!cfg.enabled()){
    Response r; r.status=503; r.json(503,"{\"code\":\"R2_NOT_CONFIGURED\",\"error\":\"Cloudflare R2 tidak dikonfigurasi.\"}");
    return r;
  }
  std::string key=r2::object_key_for_app("2.7.2","student");
  std::string url=r2::presign_url(cfg,key,3600);
  Response r; r.status=302; r.headers["Location"]=url; return r;
}
Response download_system_app(const Request& req){
  auto it=req.params.find("id");
  if(it==req.params.end()){ Response r; r.status=404; r.json(404,"{\"error\":\"not found\"}"); return r; }
  r2::R2Config cfg{"938c2a78e0f549419cc797d154904939","4ab5063738942a24451dbb0da44a110047eacdbf95eafbf19c92479d35e0dd3c","https://c5a009838cc2bfc1bec1ae19c17f28ff.r2.cloudflarestorage.com","examvan-bucket"};
  if(!cfg.enabled()){
    Response r; r.status=503; r.json(503,"{\"code\":\"R2_NOT_CONFIGURED\"}"); return r;
  }
  std::string key="apps/android/"+it->second+"/app.apk";
  std::string url=r2::presign_url(cfg,key,3600);
  Response r; r.status=302; r.headers["Location"]=url; return r;
}
} // namespace examvan::handlers::public_
