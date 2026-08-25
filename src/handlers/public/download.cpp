#include "handlers/public/download.hpp"
#include "handlers/r2/r2.hpp"
namespace examvan::handlers::public_ {
Response download_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Download EXAMVAN</h1></body></html>"; return r;
}
Response download_apk(const Request& req){
  (void)req;
  Response r; r.status=302; r.headers["Location"]="https://r2.example.com/bucket/apps/android/2.7.2/app.apk"; return r;
}
Response download_system_app(const Request& req){
  auto it=req.params.find("id");
  if(it==req.params.end()){ Response r; r.status=404; r.json(404,"{\"error\":\"not found\"}"); return r; }
  Response r; r.status=302; r.headers["Location"]="/download/apk?file="+it->second; return r;
}
} // namespace examvan::handlers::public_
