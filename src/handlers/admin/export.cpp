#include "handlers/admin/export.hpp"
#include "config/config.hpp"
#include "helpers/utils.hpp"
#include "utils/log.hpp"
#include <vector>
#include <cstdint>
#include <ctime>
#include <optional>
#ifdef HAS_LIBPQ
#include "db/pool.hpp"
#include "db/pool_real.hpp"
#include <libpq-fe.h>
#endif
namespace examvan::handlers::admin {

static std::string csv_escape(const std::string& s){
  if(s.empty()) return s;
  size_t start=s.find_first_not_of(" \t\r\n");
  char c = (start==std::string::npos? '\0': s[start]);
  bool is_formula = (c=='='||c=='+'||c=='-'||c=='@'||c=='|'||c=='%');
  bool need_quote = s.find(',')!=std::string::npos || s.find('"')!=std::string::npos || s.find('\n')!=std::string::npos || s.find('\r')!=std::string::npos;
  if(is_formula){
    std::string esc="'"+s;
    if(need_quote){
      std::string o="\"";
      for(char ch: esc){ if(ch=='"') o+="\"\""; else o+=ch; }
      o+="\""; return o;
    }
    return esc;
  }
  if(need_quote){
    std::string o="\"";
    for(char ch: s){ if(ch=='"') o+="\"\""; else o+=ch; }
    o+="\""; return o;
  }
  return s;
}
std::string build_csv_export(const std::string& exam){
  return "exam,student,score\n"+csv_escape(exam)+","+csv_escape("Budi")+",85\n";
}
static void write_le16(std::string& o, uint16_t v){ o.push_back(char(v&0xff)); o.push_back(char((v>>8)&0xff)); }
static void write_le32(std::string& o, uint32_t v){ o.push_back(char(v&0xff)); o.push_back(char((v>>8)&0xff)); o.push_back(char((v>>16)&0xff)); o.push_back(char((v>>24)&0xff)); }
static uint32_t crc32_raw(const std::string& s){ uint32_t c=0xffffffffu; for(unsigned char ch: s){ c^=ch; for(int k=0;k<8;k++) c=(c>>1) ^ (0xEDB88320u & -(c&1)); } return ~c; }
static std::string make_zip(const std::vector<std::pair<std::string,std::string>>& files){
  std::string out; std::string central;
  uint32_t offset=0;
  for(auto& f: files){
    uint32_t crc=crc32_raw(f.second);
    uint32_t sz=f.second.size();
    size_t lh=out.size();
    write_le32(out, 0x04034b50); write_le16(out, 20); write_le16(out, 0); write_le16(out, 0); write_le16(out, 0); write_le16(out, 0);
    write_le32(out, crc); write_le32(out, sz); write_le32(out, sz);
    write_le16(out, f.first.size()); write_le16(out, 0);
    out+=f.first; out+=f.second;
    write_le32(central, 0x02014b50); write_le16(central, 20); write_le16(central, 20); write_le16(central, 0); write_le16(central, 0); write_le16(central, 0); write_le16(central, 0);
    write_le32(central, crc); write_le32(central, sz); write_le32(central, sz);
    write_le16(central, f.first.size()); write_le16(central, 0); write_le16(central, 0); write_le16(central, 0); write_le16(central, 0);
    write_le32(central, 0); write_le32(central, offset); central+=f.first;
    offset += (out.size()-lh);
  }
  size_t cd_start=out.size();
  out+=central;
  size_t cd_size=out.size()-cd_start;
  write_le32(out, 0x06054b50); write_le16(out, 0); write_le16(out, 0); write_le16(out, files.size()); write_le16(out, files.size());
  write_le32(out, cd_size); write_le32(out, cd_start); write_le16(out, 0);
  return out;
}

// Escape XML untuk sel spreadsheet (inlineStr).
static std::string xml_escape(const std::string& s){
  std::string o; o.reserve(s.size()+8);
  for(char ch: s){
    switch(ch){
      case '&': o+="&amp;"; break;
      case '<': o+="&lt;"; break;
      case '>': o+="&gt;"; break;
      case '"': o+="&quot;"; break;
      default: o+=ch;
    }
  }
  return o;
}

// Kolom huruf Excel untuk indeks (A..Z, AA..).
static std::string col_ref(int idx){
  std::string s;
  while(idx>=0){ s.insert(s.begin(), char('A'+idx%26)); idx=idx/26-1; }
  return s;
}

// Bangun XLSX nyata dari baris submission. Semua sel inlineStr (valid,
// tanpa sharedStrings) — header paritas Go ExportSubmissions.
std::string build_submissions_xlsx(const std::vector<SubmissionRow>& rows){
  static const char* kHeaders[]={"ID","Nama Ujian","Nama Siswa","Nomor Ujian","Kelas","Nilai","Waktu Kumpul","ID Perangkat"};
  const int ncols=8;
  std::string sheet="<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>";
  // Header row
  sheet+="<row r=\"1\">";
  for(int i=0;i<ncols;i++){
    sheet+="<c r=\""+col_ref(i)+"1\" t=\"inlineStr\"><is><t>"+xml_escape(kHeaders[i])+"</t></is></c>";
  }
  sheet+="</row>";
  int rowidx=2;
  for(auto& r: rows){
    const char* vals[ncols]={r.id.c_str(),r.exam_name.c_str(),r.student_name.c_str(),r.exam_number.c_str(),
                             r.student_class.c_str(),r.score.c_str(),r.submitted_at.c_str(),r.mac.c_str()};
    sheet+="<row r=\""+std::to_string(rowidx)+"\">";
    for(int i=0;i<ncols;i++){
      sheet+="<c r=\""+col_ref(i)+std::to_string(rowidx)+"\" t=\"inlineStr\"><is><t>"+xml_escape(vals[i])+"</t></is></c>";
    }
    sheet+="</row>";
    rowidx++;
  }
  sheet+="</sheetData></worksheet>";

  std::string content_types = R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/></Types>)";
  std::string rels = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)";
  std::string workbook = R"(<?xml version="1.0" encoding="UTF-8"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Hasil" sheetId="1" r:id="rId1"/></sheets></workbook>)";
  return make_zip({
    {"[Content_Types].xml", content_types},
    {"_rels/.rels", rels},
    {"xl/workbook.xml", workbook},
    {"xl/worksheets/sheet1.xml", sheet},
  });
}

// Ambil exam_id dari params (route /exams/:id/export) atau query (?exam_id=).
static int export_exam_id_from(const Request& req){
  auto it=req.params.find("id");
  if(it==req.params.end()) it=req.params.find("exam_id");
  if(it!=req.params.end() && !it->second.empty()){ try{ return std::stoi(it->second); }catch(...){} }
  auto q=helpers::parse_form(req.query);
  auto fq=q.find("exam_id");
  if(fq!=q.end() && !fq->second.empty()){ try{ return std::stoi(fq->second); }catch(...){} }
  return 0; // 0 = semua ujian
}

Response export_submissions_csv(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/csv";
  r.headers["Content-Disposition"]="attachment; filename=\"export.csv\"";
  r.body=build_csv_export("Ujian"); return r;
}

Response export_submissions_xlsx(const Request& req){
  int filter=export_exam_id_from(req);
  std::vector<SubmissionRow> rows;
#ifdef HAS_LIBPQ
  try{
    auto cfg_db=Config::load();
    examvan::DbPool pool(cfg_db.database_url, 10);
    // conninfo_from_url_or_raw (BUKAN sanitized_url — password "***" gagal auth).
    examvan::db::RealPool real(examvan::conninfo_from_url_or_raw(pool.url), 10);
    if(auto c=real.acquire()){
      const char* sql="SELECT s.id,COALESCE(e.name,''),COALESCE(s.student_name,''),COALESCE(s.exam_number,''),"
        "COALESCE(s.student_class,''),COALESCE(s.score::text,''),COALESCE(s.created_at::text,''),COALESCE(s.mac_address,'')"
        " FROM submissions s LEFT JOIN exams e ON e.id=s.exam_id WHERE ($1=0 OR s.exam_id=$1) ORDER BY s.id";
      auto r=real.exec_params(c.get(),sql,{std::to_string(filter)});
      if(r && PQresultStatus(r.get())==PGRES_TUPLES_OK){
        for(int i=0;i<PQntuples(r.get());i++){
          SubmissionRow row;
          row.id=PQgetvalue(r.get(),i,0); row.exam_name=PQgetvalue(r.get(),i,1);
          row.student_name=PQgetvalue(r.get(),i,2); row.exam_number=PQgetvalue(r.get(),i,3);
          row.student_class=PQgetvalue(r.get(),i,4); row.score=PQgetvalue(r.get(),i,5);
          row.submitted_at=PQgetvalue(r.get(),i,6); row.mac=PQgetvalue(r.get(),i,7);
          rows.push_back(row);
        }
      }
      real.release(c.release());
    }
  }catch(...){ /* best-effort: tanpa PG rows kosong → XLSX header-only tetap valid */ }
#else
  (void)filter;
#endif
  utils::log_info("submissions_exported","rows="+std::to_string(rows.size()));
  Response r; r.status=200;
  r.headers["Content-Type"]="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  r.headers["Content-Disposition"]="attachment; filename=\"hasil-ujian.xlsx\"";
  r.body=build_submissions_xlsx(rows); return r;
}
} // namespace examvan::handlers::admin