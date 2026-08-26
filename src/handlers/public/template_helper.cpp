#include "handlers/public/template_helper.hpp"
#include <fstream>
#include <sstream>
namespace examvan::handlers::public_ {
std::string render_public_template(const std::string& name, const std::string& version){
  auto try_file=[&](const std::string& n)->std::string{
    std::string rpath="templates/public/"+n+".rendered.html";
    std::ifstream fr(rpath);
    if(fr){
      std::ostringstream ss; ss<<fr.rdbuf();
      std::string h=ss.str();
      size_t p=h.find("2.7.3"); if(p!=std::string::npos) h.replace(p,5,version);
      p=h.find("2.7.2"); if(p!=std::string::npos) h.replace(p,5,version);
      return h;
    }
    return "";
  };
  std::string h=try_file(name);
  if(!h.empty()) return h;
  std::string alt=name;
  for(auto &c: alt) if(c=='_') c='-'; else if(c=='-') c='_';
  h=try_file(alt);
  if(!h.empty()) return h;
  std::string path="templates/public/"+name+".html";
  std::ifstream f(path);
  if(!f){
    std::string apath="templates/public/"+alt+".html";
    f.open(apath);
    if(!f) return "";
  }
  std::ostringstream ss; ss<<f.rdbuf();
  std::string html=ss.str();
  std::ifstream sf("templates/public/shared.html");
  if(sf){
    std::ostringstream sfs; sfs<<sf.rdbuf();
    std::string shared=sfs.str();
    auto extract=[&](const std::string& n)->std::string{
      std::string s="{{ define \""+n+"\" }}"; std::string e="{{ end }}";
      size_t a=shared.find(s); if(a==std::string::npos) return ""; a+=s.size(); size_t b=shared.find(e,a); if(b==std::string::npos) return ""; return shared.substr(a,b-a);
    };
    std::string head=extract("public_head");
    std::string foot=extract("public_foot");
    size_t p;
    p=html.find("{{ template \"public_head\" . }}"); if(p!=std::string::npos) html.replace(p,28,head);
    p=html.find("{{ template \"public_foot\" . }}"); if(p!=std::string::npos) html.replace(p,28,foot);
  }
  auto repl=[&](const std::string& from, const std::string& to){
    size_t p=0; while((p=html.find(from,p))!=std::string::npos){ html.replace(p,from.size(),to); p+=to.size(); }
  };
  repl("{{.version}}",version); repl("{{ .version }}",version); repl("{{ version }}",version);
  repl("{{.seo_title}}","EXAMVAN - Aplikasi Ujian Online Aman & Tertib");
  repl("{{ .seo_title }}","EXAMVAN - Aplikasi Ujian Online Aman & Tertib");
  return html;
}
}
