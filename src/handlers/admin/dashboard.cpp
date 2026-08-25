#include "handlers/admin/dashboard.hpp"
namespace examvan::handlers::admin {
Response dashboard_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Dashboard</h1></body></html>"; return r;
}
Response dashboard_stats(const Request&){
  Response r; r.json(200,"{\"exams\":0,\"submissions\":0}"); return r;
}
} // namespace examvan::handlers::admin
