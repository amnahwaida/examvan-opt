#include "handlers/public/hasil.hpp"
namespace examvan::handlers::public_ {
Response cek_hasil_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Cek Hasil</h1><form action='/hasil' method='get'></form></body></html>";
  return r;
}
Response hasil_page(const Request& req){
  auto it=req.params.find("token");
  std::string token=it!=req.params.end()?it->second:"";
  if(token.empty()){ Response r; r.status=404; r.body="not found"; return r; }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Hasil "+token+"</h1></body></html>";
  return r;
}
Response cek_hasil_api(const Request& req){
  (void)req;
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
} // namespace examvan::handlers::public_
