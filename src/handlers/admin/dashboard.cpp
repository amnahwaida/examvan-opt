#include "handlers/admin/dashboard.hpp"
#include "handlers/admin/template_helper.hpp"
#include "store/exam_store.hpp"
#include "store/exam_store_memory.hpp"
#include "utils/sanitize.hpp"
#include "middleware/protobuf.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#include <sstream>
namespace examvan::handlers::admin {

namespace {
// Bangun baris tabel ujian dari live store. Dipanggil hanya saat ada >=1 exam.
// Konten ujian (nama/token) di-escape via examvan::html_escape (SSR aman).
std::string build_exam_table_html(const std::vector<models::Exam>& exams){
  std::ostringstream o;
  o << "<div class=\"table-responsive\">\n"
    << "<table class=\"exam-table table-with-checkbox\" id=\"examTable\">\n"
    << "<thead><tr>\n"
    << "<th scope=\"col\" style=\"width:40px;text-align:center;\"></th>\n"
    << "<th scope=\"col\">Nama Ujian</th>\n"
    << "<th scope=\"col\">Token</th>\n"
    << "<th scope=\"col\">Status</th>\n"
    << "<th scope=\"col\">Aksi</th>\n"
    << "</tr></thead>\n<tbody>\n";
  for(const auto& e: exams){
    o << "<tr id=\"exam-row-" << e.id << "\">\n"
      << "<td data-label=\"\"><input type=\"checkbox\" class=\"exam-checkbox exam-checkbox-input\" value=\""
      << e.id << "\" data-name=\"" << html_escape(e.name) << "\" data-status=\""
      << html_escape(e.status) << "\"></td>\n";
    o << "<td class=\"td-name\" data-label=\"Nama Ujian\"><a href=\"/admin/submissions?exam_id="
      << e.id << "\" class=\"exam-link\" title=\"Lihat Hasil Ujian\">" << html_escape(e.name) << "</a></td>\n";
    o << "<td class=\"td-token\" data-label=\"Token\"><code class=\"token-code\" id=\"token-"
      << e.id << "\" data-token=\"" << html_escape(e.token) << "\" data-action=\"token-copy\" role=\"button\" tabindex=\"0\" title=\"Klik untuk Salin Token\">"
      << html_escape(e.token) << "</code></td>\n";
    o << "<td data-label=\"Status\">";
    if(e.status=="active"){
      o << "<span class=\"status-badge status-active\" id=\"status-" << e.id
        << "\" data-action=\"exam-toggle-status\" data-exam-id=\"" << e.id
        << "\" tabindex=\"0\" role=\"button\" aria-pressed=\"true\" aria-label=\"Ubah status ujian "
        << html_escape(e.name) << "\" title=\"Klik untuk menonaktifkan ujian\">Aktif</span>";
    } else {
      o << "<span class=\"status-badge status-inactive\" id=\"status-" << e.id
        << "\" data-action=\"exam-toggle-status\" data-exam-id=\"" << e.id
        << "\" tabindex=\"0\" role=\"button\" aria-pressed=\"false\" aria-label=\"Ubah status ujian "
        << html_escape(e.name) << "\" title=\"Klik untuk mengaktifkan ujian\">Nonaktif</span>";
    }
    o << "</td>\n";
    o << "<td class=\"td-actions\" data-label=\"\"><div class=\"actions-group\">\n"
      << "<button class=\"btn-sm btn-questions btn-sm-compact\" data-exam-id=\"" << e.id
      << "\" data-exam-name=\"" << html_escape(e.name) << "\" data-action=\"questions-open\" title=\"Atur Soal & Kunci Jawaban\" aria-label=\"Atur soal dan kunci jawaban\"><svg class=\"icon-svg\" aria-hidden=\"true\"><use href=\"#hi-settings\"/></svg></button>\n"
      << "<div class=\"exam-action-dropdown\"><button class=\"btn-sm btn-more btn-sm-compact\" data-action=\"row-dropdown-toggle\" data-exam-id=\""
      << e.id << "\" title=\"Menu Tindakan Lainnya\" aria-label=\"Tindakan lainnya\" aria-haspopup=\"true\" aria-expanded=\"false\"><svg class=\"icon-svg\" aria-hidden=\"true\"><use href=\"#hi-chevron-down\"/></svg></button>\n"
      << "<div class=\"exam-action-dropdown-content\" id=\"dropdown-content-" << e.id << "\">\n"
      << "<button data-exam-id=\"" << e.id << "\" data-exam-name=\"" << html_escape(e.name)
      << "\" data-action=\"edit-exam-open\"><svg class=\"icon-svg\"><use href=\"#hi-edit\"/></svg> Edit Nama &amp; PDF</button>\n"
      << "<button data-exam-id=\"" << e.id << "\" data-exam-name=\"" << html_escape(e.name)
      << "\" data-action=\"exam-delete\" class=\"dropdown-toggle-btn\" style=\"color:var(--color-danger-bright);\"><svg class=\"icon-svg\"><use href=\"#hi-trash\"/></svg> Hapus Ujian</button>\n"
      << "</div></div></div></td>\n</tr>\n";
  }
  o << "</tbody>\n</table>\n</div>\n";
  return o.str();
}
} // namespace

Response dashboard_page(const Request& req){
  std::string html=render_admin_template("dashboard","2.7.2");
  if(!html.empty()){
    // Render daftar ujian LIVE dari in-memory store, ganti empty-state statis.
    // (Sebelumnya dashboard.rendered.html = snapshot statis dgn empty-state,
    //  sehingga ujian baru tak pernah muncul di tabel.)
    auto exams=store::active_store()->list_all();
    if(!exams.empty()){
      std::string table_html=build_exam_table_html(exams);
      static const std::string empty_marker="<div class=\"empty-state\">";
      size_t p=html.find(empty_marker);
      if(p!=std::string::npos){
        size_t end=html.find("</div>",p);
        // hapus blok empty-state (sampai </div> penutup setelah </p>)
        if(end!=std::string::npos){
          size_t first_close=html.find("</p>",p);
          size_t close_end=(first_close!=std::string::npos)?html.find("</div>",first_close):end;
          if(close_end==std::string::npos) close_end=end;
          close_end+=6; // "len(</div>)"
          html.replace(p,close_end-p,table_html);
        }
      }
    }
    auto it=req.headers.find("X-User");
    if(it!=req.headers.end()) html+=html_escape(it->second);
    Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.body=html; return r;
  }
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body="<html><body><h1>Dashboard</h1></body></html>"; return r;
}
Response dashboard_stats(const Request& req){
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::DashboardStats pb;
    pb.set_success(true);
    pb.set_total(0);
    pb.set_active(0);
    pb.set_storage_mb(0);
    pb.set_exams(0);
    pb.set_submissions(0);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"data\":{\"total\":0,\"active\":0,\"storage_mb\":0,\"exams\":0,\"submissions\":0},\"exams\":0,\"submissions\":0}"); return r;
}
} // namespace examvan::handlers::admin
