#include "handlers/auth/template_renderer.hpp"
#include "session/csrf.hpp"
#include "config/config.hpp"
#include "utils/sanitize.hpp"
#include <fstream>
#include <sstream>
#include <cctype>

namespace examvan::handlers::auth {

namespace {

void repl_all(std::string& h, const std::string& from, const std::string& to){
  size_t p=0;
  while((p=h.find(from,p))!=std::string::npos){ h.replace(p,from.size(),to); p+=to.size(); }
}

std::string shared_partial(const std::string& name){
  std::ifstream sf("templates/public/shared.html");
  if(!sf) return "";
  std::ostringstream ss; ss<<sf.rdbuf();
  std::string shared=ss.str();
  std::string s="{{ define \""+name+"\" }}";
  std::string e="{{ end }}";
  size_t a=shared.find(s);
  if(a==std::string::npos) return "";
  a+=s.size();
  size_t b=shared.find(e,a);
  if(b==std::string::npos) return "";
  return shared.substr(a,b-a);
}

// Cari {{end}} penutup milik marker {{if ...}} (nesting-aware).
size_t find_matching_end(const std::string& h, size_t start){
  size_t depth=1;
  size_t i=start;
  while(i<h.size()){
    size_t ni=h.find("{{", i);
    if(ni==std::string::npos) break;
    size_t nj=h.find("}}", ni);
    if(nj==std::string::npos) break;
    std::string tok=h.substr(ni+2, nj-(ni+2));
    size_t t0=tok.find_first_not_of(" \t");
    if(t0==std::string::npos){ i=nj+2; continue; }
    std::string t=tok.substr(t0);
    if(t.rfind("if ",0)==0 || t.rfind("if.",0)==0 || t=="if"){
      depth++;
    } else if(t.rfind("end",0)==0){
      depth--;
      if(depth==0) return ni;
    }
    i=nj+2;
  }
  return std::string::npos;
}

// Resolusi {{if .field}}...{{else}}...{{end}}: keep=true → sisi if, false → sisi else.
void resolve_if_block(std::string& h, const std::string& field, bool keep){
  std::string marker="{{if ."+field+"}}";
  size_t p=0;
  while((p=h.find(marker,p))!=std::string::npos){
    size_t cont=h.find("{{else}}", p);
    size_t endp=find_matching_end(h, p+marker.size());
    if(endp==std::string::npos) break;
    size_t content_start=p+marker.size();
    size_t content_end=(cont!=std::string::npos && cont<endp)?cont:endp;
    std::string keep_str;
    if(keep){
      keep_str=h.substr(content_start, content_end-content_start);
    } else if(cont!=std::string::npos && cont<endp){
      keep_str=h.substr(cont+std::string("{{else}}").size(), endp-(cont+std::string("{{else}}").size()));
    }
    h.replace(p, endp+std::string("{{end}}").size()-p, keep_str);
    p+=keep_str.size();
  }
}

// {{range .flashes}}...{{.}}...{{end}} → satu blok berisi baris flash ({{.}}
// diganti tiap baris; baris kosong di-skip).
void resolve_flashes(std::string& h, const std::vector<std::string>& flashes){
  std::string marker="{{range .flashes}}";
  size_t p=0;
  while((p=h.find(marker,p))!=std::string::npos){
    size_t endp=h.find("{{end}}", p+marker.size());
    if(endp==std::string::npos) break;
    std::string tmpl=h.substr(p+marker.size(), endp-(p+marker.size()));
    std::string out;
    for(const auto& f: flashes){
      std::string line=tmpl;
      repl_all(line, "{{.}}", html_escape(f));
      out+=line;
    }
    h.replace(p, endp+std::string("{{end}}").size()-p, out);
    p+=out.size();
  }
}

} // namespace

RenderedAuthPage render_auth_page(const PublicAuthPage& page){
  RenderedAuthPage out;

  // 1. Baca template SOURCE (nama underscore, paritas file Go).
  std::string name=page.name;
  std::ifstream f("templates/public/"+name+".html");
  if(!f){
    // Fallback: file rendered (untuk halaman yang tidak punya source).
    f.open("templates/public/"+name+".rendered.html");
    if(!f){
      out.body="<html><body>template missing: "+name+"</body></html>";
      return out;
    }
  }
  std::ostringstream ss; ss<<f.rdbuf();
  std::string h=ss.str();

  // 2. Buang komentar Go {{/* ... */}} (multi-baris).
  {
    size_t p=0;
    while((p=h.find("{{/*",p))!=std::string::npos){
      size_t e=h.find("*/}}",p);
      if(e==std::string::npos) break;
      h.erase(p, e+4-p);
    }
  }

  // 3. Resolusi blok kondisional (harus sebelum placeholder nilai).
  resolve_if_block(h, "error", !page.error.empty());
  resolve_if_block(h, "flashes", !page.flashes.empty());
  resolve_if_block(h, "username", !page.username.empty());
  resolve_if_block(h, "email_enabled", page.email_enabled);
  resolve_if_block(h, "turnstile_enabled", page.turnstile_enabled);
  resolve_if_block(h, "form_username", !page.form_username.empty());
  resolve_if_block(h, "form_email", !page.form_email.empty());
  resolve_if_block(h, "seo_index", page.seo_index);
  resolve_flashes(h, page.flashes);

  // 4. Partial: public_auth_nav → public_nav (alias di shared.html).
  repl_all(h, "{{ template \"public_auth_nav\" . }}", shared_partial("public_nav"));
  repl_all(h, "{{ template \"public_fonts\" . }}", shared_partial("public_fonts"));
  repl_all(h, "{{ template \"public_skip_link\" . }}", shared_partial("public_skip_link"));

  // 5. Nilai (semua di-html-escape kecuali markup aman yang kita kendalikan).
  std::string csrf=page.csrf_token.empty()? generate_csrf_token() : page.csrf_token;
  repl_all(h, "{{.csrf_token}}", csrf);
  repl_all(h, "{{ .csrf_token }}", csrf);
  repl_all(h, "CSRF_PLACEHOLDER", csrf);
  repl_all(h, "{{.error}}", html_escape(page.error));
  repl_all(h, "{{ .error }}", html_escape(page.error));
  repl_all(h, "{{.username}}", html_escape(page.username));
  repl_all(h, "{{ .username }}", html_escape(page.username));
  repl_all(h, "{{.email}}", html_escape(page.email));
  repl_all(h, "{{ .email }}", html_escape(page.email));
  repl_all(h, "{{.masked_email}}", html_escape(page.masked_email));
  repl_all(h, "{{ .masked_email }}", html_escape(page.masked_email));
  repl_all(h, "{{.form_username}}", html_escape(page.form_username));
  repl_all(h, "{{.form_email}}", html_escape(page.form_email));
  repl_all(h, "{{.turnstile_site_key}}", html_escape(page.turnstile_site_key));
  repl_all(h, "{{ .turnstile_site_key }}", html_escape(page.turnstile_site_key));
  repl_all(h, "{{.footer_text}}", html_escape(page.footer_text));
  repl_all(h, "{{ .footer_text }}", html_escape(page.footer_text));
  repl_all(h, "{{.seo_description}}", html_escape(page.seo_description));
  repl_all(h, "{{.seo_title}}", html_escape(page.seo_title));
  repl_all(h, "{{ .seo_title }}", html_escape(page.seo_title));
  repl_all(h, "{{.version}}", "2.7.2");
  repl_all(h, "{{ .version }}", "2.7.2");
  repl_all(h, "{{ version }}", "2.7.2");

  // 6. Meta csrf-token (attribute content) bila belum terisi.
  {
    std::string needle="csrf-token\" content=\"";
    size_t pos=0;
    while((pos=h.find(needle,pos))!=std::string::npos){
      size_t q1=h.find('"',pos+needle.size()-1);
      if(q1==std::string::npos) break;
      size_t q2=h.find('"',q1+1);
      if(q2==std::string::npos) break;
      h.replace(q1+1,q2-q1-1,csrf);
      pos=q2+1;
    }
  }

  // 7. Set-Cookie CSRF (double-submit cookie pattern) bila belum ada di body.
  out.body=h;
  out.set_cookie="csrf_token="+csrf+"; Path=/; SameSite=Lax";
  if(!Config::load().is_development()) out.set_cookie+="; Secure";
  return out;
}

} // namespace examvan::handlers::auth