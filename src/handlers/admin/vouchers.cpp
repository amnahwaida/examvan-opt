#include "handlers/admin/vouchers.hpp"
#include "helpers/utils.hpp"
namespace examvan::handlers::admin {
Response list_vouchers(const Request&){
  Response r; r.json(200,"{\"vouchers\":[]}"); return r;
}
Response redeem_voucher(const Request& req){
  auto form=helpers::parse_form(req.body);
  if(form.find("code")==form.end()){ Response r; r.status=400; r.json(400,"{\"error\":\"code required\"}"); return r; }
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
Response activate_voucher(const Request&){
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
Response billing_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Billing</h1></body></html>"; return r;
}
} // namespace examvan::handlers::admin
