// Pass-24 TDD remediasi Batch 2b — D3 & D4 (temuan review).
// Temuan: docs/review-temuan-2026-09-09-pass24.md
//  - D3 : edit-token — cek kolisi non-atomik (token_exists → update → claim)
//         + kegagalan claim ditelan. Dua admin set token sama → dua ujian
//         aktif berbagi token (siswa masuk ujian yang salah).
//  - D4 : R2 dimutasi SEBELUM store — delete menghapus PDF lalu store remove
//         bisa gagal (PDF orphan-satunya); update upload PDF baru sebelum
//         store update (objek R2 orphan bila store gagal); bulk_delete
//         melompati ujian tanpa R2 sementara delete tunggal menolak 503.
//
// Kontrak (RED dulu, GREEN setelah remediasi):
//  1. D3a — ExamStoreMemory::claim_token_if_absent(token, exclude_id) ATOMIK
//           di bawah lock store: false bila token dipakai exam lain.
//  2. D3b — update_exam edit-token memakai claim-SEBELUM-mutasi: source
//           handler memanggil claim_token_if_absent (bukan pasangan
//           token_exists → update → claim yang bisa race + menelan gagal).
//  3. D4a — delete_exam: store.remove() SEBELUM mutasi R2 (urutan source:
//           posisi exams().remove < posisi client.remove/g_upload_mock).
//  4. D4b — update_exam edit: upload R2 PDF baru SETELAH store update sukses
//           (urutan source: exams().update < r2 upload di jalur edit).
//  5. D4c — bulk_delete_exams: tanpa R2 terkonfigurasi → 503 konsisten
//           dengan delete tunggal (bukan diam-diam sukses 0 object).
#include <gtest/gtest.h>
#include "handlers/admin/exams.hpp"
#include "handlers/r2/r2.hpp"
#include "models/exam.hpp"
#include "store/exam_store.hpp"
#include "store/exam_store_memory.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <string>

using namespace examvan;

namespace {

// Mirip helper test lain: R2 lengkap + testmode (tanpa jaringan nyata).
void p35_set_r2_env(bool on){
  if(on){
    setenv("R2_ACCESS_KEY_ID","test",1);
    setenv("R2_SECRET_ACCESS_KEY","test",1);
    setenv("R2_ENDPOINT","https://test.r2.cloudflarestorage.com",1);
    setenv("R2_BUCKET","test",1);
    setenv("EXAMVAN_R2_TESTMODE","1",1);
  } else {
    setenv("R2_ACCESS_KEY_ID","",1);
    setenv("R2_SECRET_ACCESS_KEY","",1);
    setenv("R2_ENDPOINT","",1);
    setenv("R2_BUCKET","",1);
    setenv("EXAMVAN_R2_TESTMODE","",1);
  }
}

std::string p35_json_field(const std::string& body, const std::string& key){
  std::string needle="\""+key+"\":";
  size_t p=body.find(needle);
  if(p==std::string::npos) return "";
  size_t s=body.find_first_not_of(" \t\r\n", p+needle.size());
  if(s==std::string::npos) return "";
  size_t e=(body[s]=='"')? body.find('"',s+1) : body.find_first_of(",}",s);
  if(e==std::string::npos) return "";
  return body.substr(s+(body[s]=='"'?1:0), e-s-(body[s]=='"'?1:0));
}

}  // namespace

// ----------------------------------------------------------------------
// D3a: claim_token_if_absent atomik — kolisi terhadap exam lain → false.
TEST(P35, D3a_ClaimTokenIfAbsent_Atomic){
  store::ExamStoreMemory st;
  models::Exam other; other.id=7; other.token="AAAA1111"; other.status="inactive";
  ASSERT_TRUE(st.add(other));
  // Token dipakai exam lain (id=7) → claim untuk exam id=9 harus GAGAL.
  EXPECT_FALSE(st.claim_token_if_absent("AAAA1111", 9))
      << "claim_token_if_absent wajib menolak token exam lain (atomik)";
  // exclude_id mengenai diri sendiri → boleh re-claim (update token sama).
  EXPECT_TRUE(st.claim_token_if_absent("AAAA1111", 7))
      << "re-claim token milik exam sendiri (exclude_id) harus sukses";
  // Token bebas → claim sukses sekali, kedua kali gagal.
  EXPECT_TRUE(st.claim_token_if_absent("BBBB2222", 9));
  EXPECT_FALSE(st.claim_token_if_absent("BBBB2222", 3))
      << "token yang baru di-claim wajib terlihat atomik oleh claim berikutnya";
}

// ----------------------------------------------------------------------
// D3b: handler edit-token wajib claim-SEBELUM-mutasi via
// claim_token_if_absent — bukan token_exists → update → claim (TOCTOU +
// kegagalan claim ditelan diam-diam).
TEST(P35, D3b_EditTokenClaimsBeforeMutating){
  store::ExamStoreMemory st;
  models::Exam e1; e1.id=1; e1.token="CCCC3333"; e1.status="inactive";
  models::Exam e2; e2.id=2; e2.token="DDDD4444"; e2.status="inactive";
  ASSERT_TRUE(st.add(e1));
  ASSERT_TRUE(st.add(e2));
  // Admin A meng-claim EEEE5555 untuk exam 1 (sukses).
  ASSERT_TRUE(st.claim_token_if_absent("EEEE5555", 1));
  // Admin B (race hilang versi lama) mencoba set token sama utk exam 2 →
  // wajib ditolak ATOMIK, bukan lolos lalu ditolak add()/UNIQUE PG.
  EXPECT_FALSE(st.claim_token_if_absent("EEEE5555", 2));
  // Handler lama: token_exists(collision) → update tetap jalan → claim gagal
  // ditelan. Handler baru: satu panggilan atomik memutus race tersebut.
}

// Sumber handler: edit-token wajib memakai claim_token_if_absent.
TEST(P35, D3b_EditTokenSourceUsesAtomicClaim){
  std::ifstream f("src/handlers/admin/exams.cpp");
  ASSERT_TRUE(f.good());
  std::ostringstream ss; ss << f.rdbuf();
  std::string src=ss.str();
  EXPECT_NE(src.find("claim_token_if_absent"), std::string::npos)
      << "edit-token wajib memakai claim_token_if_absent (claim SEBELUM mutasi)";
}

// ----------------------------------------------------------------------
// D4a: delete_exam — store dulu, R2 belakangan (PDF tidak jadi orphan
// satu-satunya saat store remove gagal).
TEST(P35, D4a_DeleteRemovesStoreBeforeR2){
  std::ifstream f("src/handlers/admin/exams.cpp");
  ASSERT_TRUE(f.good());
  std::ostringstream ss; ss << f.rdbuf();
  std::string src=ss.str();
  auto fn=src.find("Response delete_exam");
  ASSERT_NE(fn, std::string::npos);
  auto store_rm=src.find("exams().remove(id)", fn);
  auto r2_rm=src.find("client.remove(key)", fn);
  ASSERT_NE(store_rm, std::string::npos) << "delete_exam wajib memanggil exams().remove";
  ASSERT_NE(r2_rm, std::string::npos) << "delete_exam wajib tetap menghapus object R2";
  EXPECT_LT(store_rm, r2_rm)
      << "D4: store.remove() wajib SEBELUM mutasi R2 (PDF jangan terhapus dulu)";
}

// ----------------------------------------------------------------------
// D4b: update edit — PDF baru di-upload SETELAH store update sukses.
TEST(P35, D4b_EditUploadsPdfAfterStoreUpdate){
  std::ifstream f("src/handlers/admin/exams.cpp");
  ASSERT_TRUE(f.good());
  std::ostringstream ss; ss << f.rdbuf();
  std::string src=ss.str();
  auto fn=src.find("Response update_exam");
  ASSERT_NE(fn, std::string::npos);
  auto store_upd=src.find("found = exams().update(id,", fn);
  auto r2_up=src.find("client.upload(key, edit_pdf_data)", fn);
  ASSERT_NE(store_upd, std::string::npos);
  ASSERT_NE(r2_up, std::string::npos) << "jalur edit wajib tetap upload R2";
  EXPECT_LT(store_upd, r2_up)
      << "D4: upload PDF baru wajib SETELAH exams().update sukses (objek R2 jangan orphan)";
}

// ----------------------------------------------------------------------
// D4c: bulk_delete tanpa R2 → 503 konsisten dengan delete tunggal.
TEST(P35, D4c_BulkDeleteWithoutR2Refused){
  handlers::admin::clear_exams_for_testing();
  handlers::admin::set_upload_mock_for_test(nullptr);
  p35_set_r2_env(false);
  Request c1; c1.body="name=BDnrA&file_path=a.pdf&size_bytes=100";
  auto r1=handlers::admin::create_exam(c1);
  ASSERT_EQ(r1.status,201) << r1.body;
  int id1=std::stoi(p35_json_field(r1.body,"id"));
  Request bd; bd.body="{\"ids\":["+std::to_string(id1)+"]}";
  auto res=handlers::admin::bulk_delete_exams(bd);
  EXPECT_EQ(res.status,503)
      << "bulk_delete tanpa R2 wajib 503 (konsisten delete tunggal), bukan sukses palsu: " << res.body;
  EXPECT_NE(res.body.find("R2_NOT_CONFIGURED"), std::string::npos) << res.body;
  EXPECT_TRUE(examvan::store::active_store()->get_by_id(id1).has_value())
      << "exam wajib tetap ada bila object R2 tidak bisa dibersihkan";
  p35_set_r2_env(true);
}
