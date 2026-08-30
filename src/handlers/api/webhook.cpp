#include "handlers/api/webhook.hpp"
#include "middleware/protobuf.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
namespace examvan::handlers::api {
Response webhook(const Request& req){
  if(req.body.empty()){
#ifdef HAS_PROTOBUF
    if(middleware::is_protobuf_accept(req)){
      examvan::v1::WebhookResponse pb; pb.set_success(false); pb.set_status("empty");
      std::string out; pb.SerializeToString(&out);
      Response r; r.status=400; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
    }
#endif
    Response r; r.status=400; r.json(400,"{\"error\":\"empty\"}"); return r;
  }
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::WebhookResponse pb; pb.set_success(true); pb.set_status("ok");
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
} // namespace examvan::handlers::api
