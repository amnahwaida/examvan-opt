#include "handlers/admin/export.hpp"
#include <vector>
#include <cstdint>
#include <ctime>
namespace examvan::handlers::admin {
static std::string csv_escape(const std::string& s){
  if(s.empty()) return s;
  char c=s[0];
  if(c=='='||c=='+'||c=='-'||c=='@'||c=='|'||c=='%'){
    return "'"+s;
  }
  if(s.find(',')!=std::string::npos || s.find('"')!=std::string::npos || s.find('\n')!=std::string::npos){
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
std::string build_xlsx_placeholder(const std::string& exam){
  std::string content_types = R"(<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/></Types>)";
  std::string rels = R"(<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)";
  std::string workbook = R"(<?xml version="1.0" encoding="UTF-8"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheets><sheet name="Sheet1" sheetId="1" r:id="rId1"/></sheets></workbook>)";
  std::string sheet = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData><row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"B1\" t=\"s\"><v>1</v></c><c r=\"C1\" t=\"s\"><v>2</v></c></row><row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>"+exam+"</t></is></c><c r=\"B2\" t=\"inlineStr\"><is><t>Budi</t></is></c><c r=\"C2\"><v>85</v></c></row></sheetData></worksheet>";
  std::string sharedStrings = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"3\"><si><t>exam</t></si><si><t>student</t></si><si><t>score</t></si></sst>";
  return make_zip({
    {"[Content_Types].xml", content_types},
    {"_rels/.rels", rels},
    {"xl/workbook.xml", workbook},
    {"xl/worksheets/sheet1.xml", sheet},
    {"xl/sharedStrings.xml", sharedStrings},
  });
}
Response export_submissions_csv(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="text/csv";
  r.headers["Content-Disposition"]="attachment; filename=\"export.csv\"";
  r.body=build_csv_export("Ujian"); return r;
}
Response export_submissions_xlsx(const Request&){
  Response r; r.status=200; r.headers["Content-Type"]="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  r.headers["Content-Disposition"]="attachment; filename=\"export.xlsx\"";
  r.body=build_xlsx_placeholder("Ujian"); return r;
}
} // namespace examvan::handlers::admin
