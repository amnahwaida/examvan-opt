#include <gtest/gtest.h>
#include "handlers/admin/export.hpp"
using namespace examvan::handlers::admin;

TEST(Export, CsvContainsHeader) {
  auto csv=build_csv_export("UAS");
  EXPECT_NE(csv.find("exam,student"), std::string::npos);
  EXPECT_NE(csv.find("UAS"), std::string::npos);
}

TEST(Export, XlsxHasPK) {
  auto x=build_submissions_xlsx({});
  EXPECT_EQ(x.substr(0,2), "PK");
  EXPECT_NE(x.find("Nama Siswa"), std::string::npos);
}

// Ekstrak isi file sheet1.xml dari zip stored (method 0) — cari nama file
// lalu baca ukuran dari local header. Dipakai untuk memeriksa XML sheet.
static std::string extract_sheet_xml(const std::string& zip){
  const std::string name="xl/worksheets/sheet1.xml";
  size_t p=0;
  while((p=zip.find("PK\x03\x04", p))!=std::string::npos){
    if(p+30+name.size()>zip.size()) break;
    uint16_t fnlen = (unsigned char)zip[p+26] | ((unsigned char)zip[p+27]<<8);
    uint16_t exlen = (unsigned char)zip[p+28] | ((unsigned char)zip[p+29]<<8);
    uint32_t csize = (unsigned char)zip[p+18] | ((unsigned char)zip[p+19]<<8)
                   | ((unsigned char)zip[p+20]<<16) | ((unsigned char)zip[p+21]<<24);
    if(zip.compare(p+30, name.size(), name)==0){
      return zip.substr(p+30+fnlen+exlen, csize);
    }
    p+=1;
  }
  return "";
}

TEST(Export, XlsxStripsIllegalXmlControlChars) {
  // Nama siswa bisa mengandung control char XML-illegal (0x01 dll) karena
  // submit_exam tidak sanitize — xml_escape harus membuangnya agar sheet1.xml
  // valid dan .xlsx bisa dibuka Excel. Zip stored (method 0) → sheet XML
  // terbaca langsung; jangan scan seluruh zip (byte biner header bisa 0x01).
  SubmissionRow r;
  r.id="1"; r.exam_name="UAS"; r.student_name="Budi\x01\x02Hacker";
  r.exam_number="N1"; r.student_class="A"; r.score="85"; r.submitted_at="2026-01-01"; r.mac="aa";
  auto x=build_submissions_xlsx({r});
  std::string sheet=extract_sheet_xml(x);
  ASSERT_FALSE(sheet.empty()) << "sheet1.xml not found in zip";
  EXPECT_EQ(sheet.find('\x01'), std::string::npos) << "illegal XML control char leaked into sheet XML";
  EXPECT_EQ(sheet.find('\x02'), std::string::npos);
  // Nama tetap ada (bagian legalnya).
  EXPECT_NE(sheet.find("Budi"), std::string::npos);
  EXPECT_NE(sheet.find("Hacker"), std::string::npos);
}

TEST(Export, HandlersReturn200) {
  examvan::Request r;
  EXPECT_EQ(export_submissions_csv(r).status, 200);
  EXPECT_EQ(export_submissions_xlsx(r).status, 200);
  EXPECT_NE(export_submissions_xlsx(r).headers["Content-Type"].find("spreadsheetml"), std::string::npos);
}
