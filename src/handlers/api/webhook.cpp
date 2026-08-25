#include "handlers/api/webhook.hpp"
namespace examvan::handlers::api {
Response webhook(const Request& req){
  if(req.body.empty()){ Response r; r.status=400; r.json(400,"{\"error\":\"empty\"}"); return r; }
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
} // namespace examvan::handlers::api
