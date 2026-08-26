#include "handlers/admin/template_helper.hpp"
#include <fstream>
#include <sstream>
namespace examvan::handlers::admin {
std::string render_admin_template(const std::string& name, const std::string& version){
  std::string rpath="templates/admin/"+name+".rendered.html";
  std::ifstream fr(rpath);
  if(fr){
    std::ostringstream ss; ss<<fr.rdbuf();
    std::string h=ss.str();
    size_t p=h.find("2.7.3"); if(p!=std::string::npos) h.replace(p,5,version);
    p=h.find("2.7.2"); if(p!=std::string::npos) h.replace(p,5,version);
    return h;
  }
  std::string path="templates/admin/"+name+".html";
  std::ifstream f(path);
  if(!f) return "";
  std::ostringstream ss; ss<<f.rdbuf();
  std::string html=ss.str();
  auto repl=[&](const std::string& from, const std::string& to){
    size_t p=0; while((p=html.find(from,p))!=std::string::npos){ html.replace(p,from.size(),to); p+=to.size(); }
  };
  repl("{{.version}}",version); repl("{{ .version }}",version);
  return html;
}
}
