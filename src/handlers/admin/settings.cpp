#include "handlers/admin/settings.hpp"
namespace examvan::handlers::admin {
Response settings_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Settings</h1></body></html>"; return r;
}
Response update_settings(const Request&){
  Response r; r.json(200,"{\"ok\":true}"); return r;
}
Response system_apps_page(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>System Apps</h1></body></html>"; return r;
}
} // namespace examvan::handlers::admin
