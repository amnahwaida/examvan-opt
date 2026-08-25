#include "middleware/body_limit.hpp"
namespace examvan::middleware {
Response body_limit(const Request& req, size_t max, std::function<Response(const Request&)> next){
  if(req.body.size()>max){ Response r; r.status=413; r.body="payload too large"; return r; }
  return next(req);
}
} // namespace examvan::middleware
