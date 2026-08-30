#include "handlers/admin/vouchers.hpp"
#include "helpers/utils.hpp"
#include "middleware/protobuf.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
namespace examvan::handlers::admin {
Response list_vouchers(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::VoucherList pb; pb.set_success(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"vouchers\":[]}"); return r;
}
Response redeem_voucher(const Request& req){
  auto form=helpers::parse_form(req.body);
  if(form.find("code")==form.end()){ Response r; r.status=400; r.json(400,"{\"error\":\"code required\"}"); return r; }
  Response r; r.json(200,"{\"success\":true,\"ok\":true}"); return r;
}
Response activate_voucher(const Request&){
  Response r; r.json(200,"{\"success\":true,\"ok\":true}"); return r;
}
Response billing_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Billing</h1></body></html>"; return r;
}
} // namespace examvan::handlers::admin
