#include "handlers/admin/settings.hpp"
#include "handlers/admin/template_helper.hpp"
namespace examvan::handlers::admin {
Response settings_page(const Request&){
  std::string html=render_admin_template("settings","2.7.2");
  if(!html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
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
