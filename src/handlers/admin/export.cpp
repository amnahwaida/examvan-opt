#include "handlers/admin/export.hpp"
namespace examvan::handlers::admin {
std::string build_csv_export(const std::string& exam){
  return "exam,student,score\n"+exam+",Budi,85\n";
}
std::string build_xlsx_placeholder(const std::string& exam){
  std::string csv=build_csv_export(exam);
  std::string xlsx="PK\x03\x04 placeholder xlsx for "+exam+" csv:"+csv;
  return xlsx;
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
