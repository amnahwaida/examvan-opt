#include "handlers/public/hasil.hpp"
#include "handlers/public/template_helper.hpp"
#include "utils/sanitize.hpp"
#include "middleware/protobuf.hpp"
#include "middleware/scoring.hpp"
#include "helpers/utils.hpp"
#include "session/cookie.hpp"
#include "config/config.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#ifdef HAS_LIBPQ
#include "db/pool_real.hpp"
#include "db/pool.hpp"
#endif
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <cmath>

namespace examvan::handlers::public_ {

static std::unordered_map<std::string, models::Exam> g_exams;
static std::mutex g_mu;

void set_exam_for_test(const std::string& token, const models::Exam& exam) {
  std::lock_guard<std::mutex> g(g_mu);
  g_exams[token] = exam;
}
void clear_exams_for_test() {
  std::lock_guard<std::mutex> g(g_mu);
  g_exams.clear();
}

namespace {

std::string json_escape(const std::string& s){
  std::string out="\"";
  for(unsigned char c: s){
    if(c=='"') out+="\\\"";
    else if(c=='\\') out+="\\\\";
    else if(c=='\b') out+="\\b";
    else if(c=='\f') out+="\\f";
    else if(c=='\n') out+="\\n";
    else if(c=='\r') out+="\\r";
    else if(c=='\t') out+="\\t";
    else if(c<0x20){ char buf[7]; snprintf(buf,sizeof(buf),"\\u%04x",c); out+=buf; }
    else out+=char(c);
  }
  out+="\"";
  return out;
}

std::string upper_trim(const std::string& s){
  std::string out;
  out.reserve(s.size());
  for(char c: s) if(!std::isspace((unsigned char)c)) out.push_back((char)std::toupper((unsigned char)c));
  return out;
}

// ===== lookup exam: test map dulu (unit test), lalu PG nyata =====
bool get_exam_by_token(const std::string& raw, models::Exam& out){
  { std::lock_guard<std::mutex> g(g_mu);
    auto f=g_exams.find(raw);
    if(f!=g_exams.end()){ out=f->second; return true; }
    // Case-insensitive: coba uppercase lalu lowercase (test-map bisa lowercase,
    // token produksi selalu uppercase).
    std::string up=upper_trim(raw);
    if(up!=raw){ auto f2=g_exams.find(up); if(f2!=g_exams.end()){ out=f2->second; return true; } }
    std::string lo=up;
    for(char& c: lo) c=(char)std::tolower((unsigned char)c);
    if(lo!=raw && lo!=up){ auto f3=g_exams.find(lo); if(f3!=g_exams.end()){ out=f3->second; return true; } }
  }
  std::string token=upper_trim(raw);
#ifdef HAS_LIBPQ
  try{
    auto cfg=Config::load();
    std::string db=cfg.database_url;
    if(db.empty()) if(auto* e=getenv("DATABASE_URL")) db=e;
    if(!db.empty()){
      std::string ci=pg_conninfo_from_url(db);
      if(ci.empty()) ci=db;
      examvan::db::RealPool real(ci, 2);
      if(real.connect()){
        if(auto c=real.acquire()){
          auto res=real.exec_params(c.get(),
            "SELECT id, name, token, public_results, show_answers, questions_json, identity_fields, created_by, delegated_to"
            " FROM exams WHERE token=$1 LIMIT 1", {token});
          if(res && PQresultStatus(res.get())==PGRES_TUPLES_OK && PQntuples(res.get())>0){
            out.id=std::stoi(PQgetvalue(res.get(),0,0));
            out.name=PQgetvalue(res.get(),0,1);
            out.token=PQgetvalue(res.get(),0,2);
            try{ out.public_results=std::stoi(PQgetvalue(res.get(),0,3)); }catch(...){ out.public_results=0; }
            try{ out.show_answers=std::stoi(PQgetvalue(res.get(),0,4)); }catch(...){ out.show_answers=0; }
            std::string qj=PQgetvalue(res.get(),0,5);
            if(!qj.empty()) out.questions_json=qj;
            std::string idf=PQgetvalue(res.get(),0,6);
            if(!idf.empty()) out.identity_fields=idf;
            return true;
          }
        }
      }
    }
  }catch(...){}
#endif
  return false;
}

bool is_logged_in(const Request& req){
  auto it=req.headers.find("Cookie");
  if(it==req.headers.end()) return false;
  auto cfg=Config::load();
  return verify_session_cookie(cfg.secret_key, it->second).has_value();
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

void repl_all(std::string& h, const std::string& from, const std::string& to){
  size_t p=0; while((p=h.find(from,p))!=std::string::npos){ h.replace(p,from.size(),to); p+=to.size(); }
}

// Cari {{end}} penutup milik marker {{if ...}} (nesting-aware untuk {{if}}).
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

// Hapus blok {{if .field}}...{{else}}...{{end}} (multi-baris, non-nesting):
// keep=true → sisakan isi sebelum {{else}}; keep=false → isi setelah {{else}} (atau kosong).
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
    if(keep) keep_str=h.substr(content_start, content_end-content_start);
    else if(cont!=std::string::npos && cont<endp) keep_str=h.substr(cont+std::string("{{else}}").size(), endp-(cont+std::string("{{else}}").size()));
    h.replace(p, endp+std::string("{{end}}").size()-p, keep_str);
    p+=keep_str.size();
  }
}

struct HasilCtx {
  std::string exam_name, token, footer_text="\u00a9 2026 EXAMVAN Team. All rights reserved.";
  std::string creator_name, delegated_name, total_students="0";
  bool error=false, is_disabled=false, is_logged_in=false, show_answers=false;
};

std::string render_hasil_html(const HasilCtx& ctx){
  std::ifstream f("templates/public/hasil.html");
  if(!f){
    std::string body=ctx.error?"Ujian tidak ditemukan":(ctx.is_disabled?"Hasil belum dipublikasikan":"Hasil "+html_escape(ctx.exam_name));
    std::string title_attr=(ctx.error||ctx.is_disabled)?"":" id=\"examTitle\"";
    return "<html><body><div id=\"main-content\"><h1"+title_attr+">"+body+"</h1><p>Token: "+html_escape(ctx.token)
      +"</p><span>Peserta: "+ctx.total_students+"</span></div></body></html>";
  }
  std::ostringstream ss; ss<<f.rdbuf();
  std::string h=ss.str();

  // Buang komentar Go template: {{/* ... */}} (multi-baris).
  {
    size_t p=0;
    while((p=h.find("{{/*",p))!=std::string::npos){
      size_t e=h.find("*/}}",p);
      if(e==std::string::npos) break;
      h.erase(p, e+4-p);
    }
  }

  repl_all(h, "{{.version}}", "2.7.2");
  repl_all(h, "{{ .version }}", "2.7.2");
  repl_all(h, "{{ version }}", "2.7.2");
  repl_all(h, "{{ template \"public_fonts\" . }}", shared_partial("public_fonts"));
  repl_all(h, "{{ template \"public_skip_link\" . }}", shared_partial("public_skip_link"));

  // Buang komentar Go template {{/* ... */}} yang ikut terbawa partial.
  {
    size_t p=0;
    while((p=h.find("{{/*",p))!=std::string::npos){
      size_t e=h.find("*/}}",p);
      if(e==std::string::npos) break;
      h.erase(p, e+4-p);
    }
  }

  // Inline boolean JS (single-line, harus diganti SEBELUM blok tiga arah).
  repl_all(h, "{{if .is_logged_in}}true{{else}}false{{end}}", ctx.is_logged_in?"true":"false");
  repl_all(h, "{{if .is_disabled}}true{{else}}false{{end}}", ctx.is_disabled?"true":"false");
  repl_all(h, "{{if .show_answers}}true{{else}}false{{end}}", ctx.show_answers?"true":"false");
  repl_all(h, "{{if .error}}true{{else}}false{{end}}", ctx.error?"true":"false");

  // Blok tiga arah utama: {{if .is_disabled}} ... {{else if .error}} ... {{else}} ... {{end}}
  {
    std::string marker="{{if .is_disabled}}";
    size_t p=h.find(marker);
    if(p!=std::string::npos){
      size_t b1=h.find("{{else if .error}}", p);
      size_t b2=h.find("{{else}}", p);
      size_t endp=find_matching_end(h, p+marker.size());
      if(endp!=std::string::npos){
        std::string keep;
        if(ctx.is_disabled){
          keep=h.substr(p+marker.size(), (b1!=std::string::npos?b1:b2)-(p+marker.size()));
        } else if(ctx.error && b1!=std::string::npos && b2!=std::string::npos){
          keep=h.substr(b1+std::string("{{else if .error}}").size(), b2-(b1+std::string("{{else if .error}}").size()));
        } else {
          keep=h.substr(b2+std::string("{{else}}").size(), endp-(b2+std::string("{{else}}").size()));
        }
        h.replace(p, endp+std::string("{{end}}").size()-p, keep);
      }
    }
  }

  // Kondisi pembuat/guru.
  resolve_if_block(h, "creator_name", !ctx.creator_name.empty());
  resolve_if_block(h, "delegated_name", !ctx.delegated_name.empty());

  // Kondisi inline satu baris di <title>/<meta>.
  resolve_if_block(h, "exam_name", !ctx.exam_name.empty());

  // Nilai.
  repl_all(h, "{{.token}}", html_escape(ctx.token));
  repl_all(h, "{{.exam_name}}", html_escape(ctx.exam_name));
  repl_all(h, "{{.total_students}}", ctx.total_students);
  repl_all(h, "{{.creator_name}}", html_escape(ctx.creator_name));
  repl_all(h, "{{.delegated_name}}", html_escape(ctx.delegated_name));
  repl_all(h, "{{.footer_text}}", html_escape(ctx.footer_text));
  return h;
}

std::string fmt_num(double v){
  char buf[32];
  snprintf(buf,sizeof(buf),"%.2f",v);
  std::string s=buf;
  while(!s.empty() && s.back()=='0') s.pop_back();
  if(!s.empty() && s.back()=='.') s.pop_back();
  return s.empty()?"0":s;
}

#ifdef HAS_LIBPQ
// ===== format waktu =====
// PG timestamptz text: "2026-09-05 10:00:00.123456+00" atau ISO "2026-09-05T10:00:00Z".
std::string iso_utc_from_pg(const std::string& raw){
  std::string s=raw;
  size_t sp=s.find(' ');
  if(sp!=std::string::npos && sp<12) s[sp]='T';
  // potong fraksi/detik ekstra: pertahankan 19 karakter pertama "YYYY-MM-DDTHH:MM:SS"
  if(s.size()>=19 && s[10]=='T'){
    std::string base=s.substr(0,19);
    if(s.size()>19 && s[19]!='Z') base+="Z";
    return base;
  }
  if(!s.empty() && s.back()!='Z') s+="Z";
  return s;
}

std::string wib_display(const std::string& raw){
  std::string iso=iso_utc_from_pg(raw);
  auto tp=helpers::parse_iso_utc(iso);
  if(!tp) return "";
  auto wib=*tp + std::chrono::minutes(420); // UTC+7
  std::time_t t=std::chrono::system_clock::to_time_t(wib);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm,&t);
#else
  gmtime_r(&t,&tm);
#endif
  char buf[32];
  std::strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M", &tm);
  return buf;
}

// ===== jawaban =====
std::map<std::string,std::string> parse_answers(const std::string& json){
  std::map<std::string,std::string> out;
  if(json.empty()) return out;
  size_t i=0, n=json.size();
  while(i<n){
    size_t q1=json.find('"', i);
    if(q1==std::string::npos) break;
    size_t q2=q1+1; while(q2<n){ if(json[q2]=='\\'){q2+=2;continue;} if(json[q2]=='"') break; q2++; }
    if(q2>=n) break;
    std::string key=json.substr(q1+1,q2-q1-1);
    size_t colon=json.find(':', q2);
    if(colon==std::string::npos) break;
    size_t v=colon+1;
    while(v<n && (json[v]==' '||json[v]=='\t'||json[v]=='\n'||json[v]=='\r')) v++;
    std::string val;
    if(v<n && json[v]=='"'){
      size_t v2=v+1; while(v2<n){ if(json[v2]=='\\'){v2+=2;continue;} if(json[v2]=='"') break; v2++; }
      val=json.substr(v+1, v2-(v+1));
      i=v2+1;
    } else {
      size_t e=v; while(e<n && json[e]!=',' && json[e]!='}') e++;
      val=json.substr(v,e-v);
      i=e;
    }
    out[key]=val;
  }
  return out;
}

std::string norm(const std::string& s){
  std::string o; o.reserve(s.size());
  for(char c: s) if(!std::isspace((unsigned char)c)) o.push_back((char)std::tolower((unsigned char)c));
  return o;
}

// evaluated_answers: {"1":{"earned":2,"statusText":"correct","statusClass":"correct"}, ...}
std::string build_evaluated_answers(const std::map<std::string,std::string>& answers, const std::vector<scoring::Question>& qs){
  std::string out="{";
  bool first=true;
  for(const auto& q: qs){
    std::string qn=std::to_string(q.number);
    auto it=answers.find(qn);
    std::string status, statusText;
    double earned=0;
    if(it==answers.end() || it->second.empty()){
      status=statusText="unanswered";
    } else if(norm(it->second)==norm(q.key)){
      status=statusText="correct";
      earned=q.weight>0?q.weight:1.0;
    } else {
      status=statusText="incorrect";
    }
    if(!first) out+=",";
    first=false;
    out+="\""+qn+"\":{\"earned\":"+fmt_num(earned)
      +",\"statusText\":\""+statusText+"\",\"statusClass\":\""+status+"\"}";
  }
  out+="}";
  return out;
}
#endif // HAS_LIBPQ

const std::string kDefaultIdentityFields =
  "[{\"key\":\"student_name\",\"label\":\"Nama\",\"required\":true},"
  "{\"key\":\"exam_number\",\"label\":\"Nomor Ujian\",\"required\":true},"
  "{\"key\":\"student_class\",\"label\":\"Kelas\",\"required\":true}]";

// Buang "key"/"answer" dari raw JSON soal bila pengunjung tak berhak.
std::string strip_sensitive_keys(const std::string& questions_json){
  std::string out=questions_json;
  for(const std::string& k: std::vector<std::string>{"key","answer"}){
    std::string needle=",\""+k+"\":";
    size_t p=0;
    while((p=out.find(needle,p))!=std::string::npos){
      size_t v=p+needle.size();
      if(v<out.size() && out[v]=='"'){
        size_t e=v+1; while(e<out.size()){ if(out[e]=='\\'){e+=2;continue;} if(out[e]=='"') break; e++; }
        out.erase(p, e+1-p);
      } else if(v<out.size()){
        size_t e=v; while(e<out.size() && out[e]!=',' && out[e]!='}') e++;
        out.erase(p, e-p);
      } else break;
    }
  }
  return out;
}

} // namespace

Response cek_hasil_page(const Request& req){
  std::string ver="2.7.2";
  auto it=req.headers.find("X-Version");
  if(it!=req.headers.end()) ver=it->second;
  std::string html=render_public_template("cek_hasil", ver);
  if(!html.empty()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body=R"html(<html><body><div id="main-content"><h1>Cek Hasil</h1><form action='/hasil' method='get'><input aria-label="Cari nama siswa" name="q"><button>Cari</button></form></div></body></html>)html";
  return r;
}

Response hasil_page(const Request& req){
  auto it=req.params.find("token");
  std::string token=it!=req.params.end()?it->second:"";
  token=upper_trim(token);
  Response base; base.headers["X-Robots-Tag"]="noindex, nofollow";
  base.headers["Cache-Control"]="no-store, no-cache, must-revalidate, max-age=0";

  HasilCtx ctx; ctx.token=token;
  if(token.empty()){
    ctx.error=true;
    Response r=base; r.status=404; r.headers["Content-Type"]="text/html";
    r.body=render_hasil_html(ctx); return r;
  }
  models::Exam exam;
  bool found=get_exam_by_token(token, exam);
  if(!found){
    ctx.error=true;
    Response r=base; r.status=404; r.headers["Content-Type"]="text/html";
    r.body=render_hasil_html(ctx); return r;
  }
  bool logged=is_logged_in(req);
  if(!exam.are_results_public() && !logged){
    ctx.is_disabled=true; ctx.exam_name=exam.name;
    Response r=base; r.status=403; r.headers["Content-Type"]="text/html";
    r.body=render_hasil_html(ctx); return r;
  }
  ctx.exam_name=exam.name;
  ctx.is_logged_in=logged;
  ctx.show_answers=exam.show_answers!=0;
#ifdef HAS_LIBPQ
  try{
    auto cfg=Config::load();
    std::string db=cfg.database_url;
    if(db.empty()) if(auto* e=getenv("DATABASE_URL")) db=e;
    if(!db.empty()){
      std::string ci=pg_conninfo_from_url(db);
      if(ci.empty()) ci=db;
      examvan::db::RealPool real(ci, 2);
      if(real.connect()){
        if(auto c=real.acquire()){
          auto cnt=real.exec_params(c.get(),
            "SELECT COUNT(*) FROM submissions WHERE exam_id=$1 AND answers_json IS NOT NULL AND answers_json != ''",
            {std::to_string(exam.id)});
          if(cnt && PQresultStatus(cnt.get())==PGRES_TUPLES_OK && PQntuples(cnt.get())>0)
            ctx.total_students=PQgetvalue(cnt.get(),0,0);
          auto cr=real.exec_params(c.get(),"SELECT username FROM admin_users WHERE id=$1",{std::to_string(exam.created_by)});
          if(cr && PQntuples(cr.get())>0) ctx.creator_name=PQgetvalue(cr.get(),0,0);
        }
      }
    }
  }catch(...){}
#endif
  Response r=base; r.status=200; r.headers["Content-Type"]="text/html";
  r.body=render_hasil_html(ctx); return r;
}

Response cek_hasil_api(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::CekHasilApiResponse pb;
    pb.set_success(true);
    pb.set_ok(true);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  auto it=req.params.find("token");
  std::string token=it!=req.params.end()?it->second:"";
  token=upper_trim(token);
  Response base; base.headers["Cache-Control"]="no-store, no-cache, must-revalidate, max-age=0";

  if(token.empty()){
    Response r=base; r.status=404;
    r.json(404,"{\"success\":false,\"message\":\"Token ujian tidak valid atau ujian tidak ditemukan.\"}"); return r;
  }
  models::Exam exam;
  if(!get_exam_by_token(token, exam)){
    Response r=base; r.status=404;
    r.json(404,"{\"success\":false,\"message\":\"Token ujian tidak valid atau ujian tidak ditemukan.\"}"); return r;
  }
  bool logged=is_logged_in(req);
  if(!exam.are_results_public() && !logged){
    Response r=base; r.status=403;
    r.json(403,"{\"success\":false,\"message\":\"Akses dinonaktifkan: Halaman hasil ujian untuk siswa dinonaktifkan oleh guru.\"}"); return r;
  }

  auto q=helpers::parse_form(req.query);
  auto getp=[&](const std::string& k)->std::string{ auto f=q.find(k); return f!=q.end()?f->second:""; };
  int page=1, per_page=100;
  try{ page=std::stoi(getp("page")); }catch(...){}
  try{ per_page=std::stoi(getp("per_page")); }catch(...){}
  if(page<1) page=1;
  if(per_page<1) per_page=1; else if(per_page>500) per_page=500;
  std::string search=getp("search");

  // Soal + max_score.
  std::string questions_json=exam.questions_json.value_or("");
  auto qs=scoring::parse_questions(questions_json);
  double max_score=0;
  for(const auto& q: qs) max_score += (q.weight>0?q.weight:1.0);
  bool show_answers=exam.show_answers!=0;
  std::string questions_out=questions_json;
  if(!show_answers && !logged && !questions_out.empty()) questions_out=strip_sensitive_keys(questions_out);
  if(questions_out.empty()) questions_out="[]";
  std::string identity_fields=exam.identity_fields.value_or("");
  if(identity_fields.empty()) identity_fields=kDefaultIdentityFields;

  std::string stats="{\"count\":0,\"average\":0,\"max\":0,\"min\":0}";
  std::string submissions="[]";
  int total=0, total_pages=1;
  bool got=false;
#ifdef HAS_LIBPQ
  try{
    auto cfg=Config::load();
    std::string db=cfg.database_url;
    if(db.empty()) if(auto* e=getenv("DATABASE_URL")) db=e;
    if(!db.empty()){
      std::string ci=pg_conninfo_from_url(db);
      if(ci.empty()) ci=db;
      examvan::db::RealPool real(ci, 2);
      if(real.connect()){
        if(auto c=real.acquire()){
          auto st=real.exec_params(c.get(),
            "SELECT COUNT(*), COALESCE(AVG(score),0), COALESCE(MAX(score),0), COALESCE(MIN(score),0)"
            " FROM submissions WHERE exam_id=$1 AND answers_json IS NOT NULL AND answers_json != ''",
            {std::to_string(exam.id)});
          if(st && PQresultStatus(st.get())==PGRES_TUPLES_OK && PQntuples(st.get())>0){
            stats="{\"count\":"+std::string(PQgetvalue(st.get(),0,0))
              +",\"average\":"+fmt_num(std::atof(PQgetvalue(st.get(),0,1)))
              +",\"max\":"+fmt_num(std::atof(PQgetvalue(st.get(),0,2)))
              +",\"min\":"+fmt_num(std::atof(PQgetvalue(st.get(),0,3)))+"}";
          }
          std::string count_sql="SELECT COUNT(*) FROM submissions WHERE exam_id=$1 AND answers_json IS NOT NULL AND answers_json != ''";
          std::vector<std::string> args={std::to_string(exam.id)};
          if(!search.empty()){ count_sql+=" AND student_name ILIKE '%'||$2||'%'"; args.push_back(search); }
          auto ct=real.exec_params(c.get(),count_sql,args);
          if(ct && PQntuples(ct.get())>0){ try{ total=std::stoi(PQgetvalue(ct.get(),0,0)); }catch(...){ total=0; } }
          total_pages=(total+per_page-1)/per_page; if(total_pages<1) total_pages=1;

          std::string sql="SELECT id, student_name, exam_number, student_class, answers_json, score, start_time, created_at, identity_data"
            " FROM submissions WHERE exam_id=$1 AND answers_json IS NOT NULL AND answers_json != ''";
          std::vector<std::string> args2={std::to_string(exam.id)};
          if(!search.empty()){ sql+=" AND student_name ILIKE '%'||$2||'%'"; args2.push_back(search); }
          sql+=" ORDER BY score DESC NULLS LAST LIMIT $"+std::to_string(args2.size()+1)+" OFFSET $"+std::to_string(args2.size()+2);
          args2.push_back(std::to_string(per_page));
          args2.push_back(std::to_string((page-1)*per_page));
          auto rows=real.exec_params(c.get(),sql,args2);
          if(rows && PQresultStatus(rows.get())==PGRES_TUPLES_OK){
            int n=PQntuples(rows.get());
            std::string s="[";
            for(int i=0;i<n;i++){
              if(i>0) s+=",";
              std::string score=PQgetvalue(rows.get(),i,5);
              std::string score_j=score.empty()?"null":fmt_num(std::atof(score.c_str()));
              std::string stime=PQgetvalue(rows.get(),i,6);
              std::string ctime=PQgetvalue(rows.get(),i,7);
              std::string answers_raw=PQgetvalue(rows.get(),i,4);
              auto answers=parse_answers(answers_raw);
              std::string eval=build_evaluated_answers(answers, qs);
              std::string max_j=max_score>0?fmt_num(max_score):"null";
              std::string idata=PQgetvalue(rows.get(),i,8);
              if(idata.empty()) idata="{}";
              s+="{\"id\":"+std::string(PQgetvalue(rows.get(),i,0))
                +",\"student_name\":"+json_escape(PQgetvalue(rows.get(),i,1))
                +",\"exam_number\":"+json_escape(PQgetvalue(rows.get(),i,2))
                +",\"student_class\":"+json_escape(PQgetvalue(rows.get(),i,3))
                +",\"identity_data\":"+idata
                +",\"score\":"+score_j
                +",\"max_score\":"+max_j
                +",\"start_time\":"+(stime.empty()?"null":json_escape(iso_utc_from_pg(stime)))
                +",\"created_at\":"+json_escape(iso_utc_from_pg(ctime))
                +",\"start_time_display\":"+json_escape(wib_display(stime))
                +",\"created_at_display\":"+json_escape(wib_display(ctime))
                +",\"evaluated_answers\":"+eval;
              if(show_answers || logged){
                s+=",\"answers\":"+(answers_raw.empty()?"{}":answers_raw);
              }
              s+="}";
            }
            s+="]";
            submissions=s;
            got=true;
          }
        }
      }
    }
  }catch(...){}
#endif
  (void)got;
  std::string max_score_j=max_score>0?fmt_num(max_score):"null";
  Response r=base; r.status=200;
  r.json(200,"{\"success\":true,\"exam_name\":"+json_escape(exam.name)
    +",\"show_answers\":"+(show_answers?"true":"false")
    +",\"token\":"+json_escape(exam.token)
    +",\"questions\":"+questions_out
    +",\"identity_fields\":"+identity_fields
    +",\"max_score\":"+max_score_j
    +",\"submissions\":"+submissions
    +",\"stats\":"+stats
    +",\"pagination\":{\"page\":"+std::to_string(page)+",\"per_page\":"+std::to_string(per_page)
    +",\"total\":"+std::to_string(total)+",\"total_pages\":"+std::to_string(total_pages)+"}}");
  return r;
}

} // namespace examvan::handlers::public_