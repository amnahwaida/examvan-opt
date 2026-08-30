#include "handlers/public/hasil.hpp"
#include "handlers/public/template_helper.hpp"
#include "utils/sanitize.hpp"
#include "middleware/protobuf.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#include <unordered_map>
#include <mutex>

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
  if(token.empty()){
    Response r; r.status=404; r.headers["Content-Type"]="text/html";
    r.body=R"html(<html><body><div id="main-content"><h1>Hasil Ujian</h1><p>Token tidak ditemukan</p></div></body></html>)html";
    return r;
  }
  models::Exam exam;
  bool found=false;
  { std::lock_guard<std::mutex> g(g_mu); auto f=g_exams.find(token); if(f!=g_exams.end()){ exam=f->second; found=true; } }
  if(!found){
    Response r; r.status=404; r.headers["Content-Type"]="text/html";
    r.body=R"html(<html><body><div id="main-content"><h1>Hasil Ujian</h1><p>Ujian tidak ditemukan</p></div></body></html>)html";
    return r;
  }
  if(!exam.are_results_public()){
    Response r; r.status=200; r.headers["Content-Type"]="text/html";
    r.body=R"html(<html><body><div id="main-content"><h1>Hasil Ujian</h1><p>Hasil belum dipublikasikan</p></div></body></html>)html";
    return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><div id=\"main-content\"><h1 id=\"examTitle\">Hasil "+html_escape(exam.name)+"</h1><p>Token: "+html_escape(token)+"</p><span>Peserta: 0</span></div></body></html>";
  return r;
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
  Response r; r.json(200,"{\"ok\":true}"); return r;
}

} // namespace examvan::handlers::public_
