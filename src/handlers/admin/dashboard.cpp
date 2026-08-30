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
    std::string display_token = e.active_token.empty()? e.token : e.active_token;
    bool tombstoned = e.tombstoned_at.has_value();
    o << "<tr id=\"exam-row-" << e.id << "\">\n"
      << "<td data-label=\"\"><input type=\"checkbox\" class=\"exam-checkbox exam-checkbox-input\" value=\""
      << e.id << "\" data-name=\"" << html_escape(e.name) << "\" data-status=\""
      << html_escape(e.status) << "\"></td>\n";
    o << "<td class=\"td-name\" data-label=\"Nama Ujian\"><a href=\"/admin/submissions?exam_id="
      << e.id << "\" class=\"exam-link\" title=\"Lihat Hasil Ujian\">" << html_escape(e.name) << "</a></td>\n";
    o << "<td class=\"td-token\" data-label=\"Token\"><div class=\"token-mode-group\">\n"
      << "<select class=\"token-mode-select\" aria-label=\"Mode token ujian\" title=\"Statis: token tetap. Dinamis: token berganti otomatis.\" data-exam-id=\""
      << e.id << "\">"
      << "<option value=\"static\"" << (e.get_token_mode()=="static"?" selected":"") << ">Statis</option>"
      << "<option value=\"dynamic\"" << (e.get_token_mode()!="static"?" selected":"") << ">Dinamis</option>"
      << "</select>\n"
      << "<div class=\"token-static-settings\" style=\"display:flex;align-items:center;gap:4px;white-space:nowrap;\">"
      << "<code class=\"token-code\" id=\"token-" << e.id << "\" data-token=\"" << html_escape(display_token)
      << "\" data-action=\"token-copy\" role=\"button\" tabindex=\"0\" title=\"Klik untuk Salin Token\">"
      << html_escape(display_token) << "</code></div>\n"
      << "</div></td>\n";
    o << "<td data-label=\"Status\">";
    if(e.status=="active"){
      o << "<span class=\"status-badge status-active\" id=\"status-" << e.id
        << "\" data-action=\"exam-toggle-status\" data-exam-id=\"" << e.id
        << "\" tabindex=\"0\" role=\"button\" aria-pressed=\"true\" aria-label=\"Ubah status ujian "
        << html_escape(e.name) << "\" title=\"Klik untuk menonaktifkan ujian\">Aktif</span>";
    } else if(tombstoned){
      o << "<span class=\"status-badge status-tombstoned\" id=\"status-" << e.id
        << "\" data-action=\"exam-toggle-status\" data-exam-id=\"" << e.id
        << "\" tabindex=\"0\" role=\"button\" aria-pressed=\"false\" aria-label=\"Aktifkan kembali ujian "
        << html_escape(e.name) << "\" title=\"Nonaktif otomatis oleh sistem. Klik untuk mengaktifkan kembali\">Nonaktif Otomatis</span>";
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
  // Fallback tetap menyediakan form upload dasar, bukan halaman kosong yang
  // membuat admin tidak bisa memakai alur create exam saat template hilang.
  Response r; r.status=200; r.headers["Content-Type"]="text/html";
  r.body=R"HTML(<!doctype html><html><head><meta charset="utf-8"><title>Dashboard</title>
<meta name="csrf-token" content=""><link rel="stylesheet" href="/static/css/admin.css"></head>
<body><main><h1>Dashboard</h1><form id="uploadForm" enctype="multipart/form-data">
<label for="examName">Nama Ujian</label><input id="examName" name="name" required>
<label for="pdfFile">PDF</label><input id="pdfFile" name="pdf_file" type="file" accept="application/pdf" data-max-mb="5" required>
<label for="customToken">Token (opsional)</label><input id="customToken" name="custom_token" maxlength="8">
<div id="uploadProgress" style="display:none"><span id="progressFill"></span><span id="progressText">0%</span></div>
<button id="btnUpload" type="submit">Upload Ujian</button></form>
<div id="toastContainer" role="status" aria-live="polite"></div></main>
<script src="/static/js/admin-core.js"></script><script src="/static/js/admin.js"></script></body></html>)HTML";
  return r;
}
Response dashboard_stats(const Request& req){
  auto exams=store::active_store()->list_all();
  int total=static_cast<int>(exams.size());
  int active=0;
  int64_t storage_bytes=0;
  for(auto& e: exams){
    if(e.is_active()) active++;
    storage_bytes+=e.size_bytes;
  }
  int storage_mb=static_cast<int>(storage_bytes/(1024*1024));
#ifdef HAS_PROTOBUF
  if(middleware::is_protobuf_accept(req)){
    examvan::v1::DashboardStats pb;
    pb.set_success(true);
    pb.set_total(total);
    pb.set_active(active);
    pb.set_storage_mb(storage_mb);
    pb.set_exams(total);
    pb.set_submissions(0);
    std::string out; pb.SerializeToString(&out);
    Response r; r.status=200; r.headers["Content-Type"]="application/x-protobuf"; r.body=out; return r;
  }
#endif
  Response r; r.json(200,"{\"success\":true,\"data\":{\"total\":"+std::to_string(total)
    +",\"active\":"+std::to_string(active)
    +",\"storage_mb\":"+std::to_string(storage_mb)
    +",\"exams\":"+std::to_string(total)
    +",\"submissions\":0},\"exams\":"+std::to_string(total)+",\"submissions\":0}"); return r;
}
} // namespace examvan::handlers::admin
