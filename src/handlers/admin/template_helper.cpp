#include "handlers/admin/template_helper.hpp"
#include "session/csrf.hpp"
#include "config/config.hpp"
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

RenderedAdminPage render_admin_page(const std::string& name, const std::string& version){
  RenderedAdminPage out;
  std::string html=render_admin_template(name, version);
  if(html.empty()){
    // Fallback minimal: halaman tanpa form tetap perlu meta csrf kosong.
    out.html=html;
    out.csrf_cookie="";
    return out;
  }
  std::string csrf=generate_csrf_token();
  auto replace_all=[&](const std::string& from){
    size_t p=0; while((p=html.find(from,p))!=std::string::npos){ html.replace(p,from.size(),csrf); p+=csrf.size(); }
  };
  replace_all("{{.csrf_token}}");
  replace_all("{{ .csrf_token }}");
  replace_all("{{.csrf_token }}");
  replace_all("{{ .csrf_token}}");
  replace_all("CSRF_PLACEHOLDER");
  auto replace_attr=[&](const std::string& needle){
    size_t pos=0;
    while((pos=html.find(needle,pos))!=std::string::npos){
      size_t q1=html.find('"',pos+needle.size()-1);
      if(q1==std::string::npos) q1=html.find('\'',pos+needle.size()-1);
      if(q1==std::string::npos) break;
      char qc=html[q1];
      size_t q2=html.find(qc,q1+1);
      if(q2==std::string::npos) break;
      html.replace(q1+1,q2-q1-1,csrf);
      pos=q2+1;
    }
  };
  replace_attr("csrf-token\" content=\"");
  replace_attr("csrf_token\" value=\"");
  replace_attr("_csrf\" value=\"");
  replace_attr("csrf-token' content='");
  replace_attr("csrf_token' value='");
  // Jika template rendered tidak punya placeholder (token hardcoded dari
  // render Go), ganti token hardcoded 64-hex itu dengan token segar.
  if(html.find(csrf)==std::string::npos){
    // ganti token hex 64 yang tampak (pola rendered Go) pada atribut csrf.
    auto replace_hex_attr=[&](const std::string& needle){
      size_t pos=0;
      while((pos=html.find(needle,pos))!=std::string::npos){
        size_t q1=html.find('"',pos+needle.size()-1);
        if(q1==std::string::npos) break;
        size_t q2=html.find('"',q1+1);
        if(q2==std::string::npos) break;
        std::string cur=html.substr(q1+1,q2-q1-1);
        if(cur.size()==64){ html.replace(q1+1,64,csrf); pos=q2+1; }
        else pos=q1+1;
      }
    };
    replace_hex_attr("csrf-token\" content=\"");
    replace_hex_attr("csrf_token\" value=\"");
    replace_hex_attr("_csrf_token\" value=\"");
  }
  std::string ck="csrf_token="+csrf+"; Path=/; SameSite=Lax";
  if(!Config::load().is_development()) ck+="; Secure";
  out.html=html;
  out.csrf_cookie=ck;
  return out;
}
}
