// Pass-22 remediasi carried-over pass-20: H7, M7, M8, L3.
//
// H7: heartbeat/exam_completed WS menolak non-privileged — keputusan paritas
//   Go (hub.go:378-385) TERVERIFIKASI: siswa melaporkan presence via HTTP
//   AccessLog (setStudentHeartbeat exams.go:1570), WS hanya untuk pengawas
//   (privileged). Gate yang benar; yang kurang: (a) komentar keputusan di
//   hub.cpp, (b) test gate utk exam_completed non-privileged (hanya heartbeat
//   yang ter-test), (c) test jalur protobuf WsEnvelope.
// M7: cek scope submission 100% di router (router_full.cpp:195) — handler
//   submissions.cpp memercayai scope router. Split PG vs memory store bisa
//   salah 403/bypass bila dua store divergen. Handler WAJIB cek scope
//   sendiri (defense-in-depth), paritas checkExamOwnership Go.
// M8: bulk-toggle/bulk-delete exams hanya cek created_by/delegated_to
//   (exams.cpp) — exam_pengawas assigned + operator same-instansi tidak
//   dihitung, beda dengan single-assign path (router scope="exam" +
//   exam_access). Paritas: bulk harus menghormati scope yang sama.
// L3: mask_token masih sized-mask (settings.cpp) — std::string(t.size()-4,'*')
//   bocor panjang secret. Wajib fixed-length.
#include <gtest/gtest.h>
#include "websocket/hub.hpp"
#include "websocket/socketio.hpp"
#ifdef HAS_PROTOBUF
#include "examvan.pb.h"
#endif
#include <fstream>
#include <sstream>
#include <string>

static std::string read_src30(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ===== H7: gate WS end-to-end ===========================================

TEST(P30, H7_HeartbeatDecisionDocumented){
  // Keputusan paritas (siswa via HTTP, WS pengawas-only) wajib terdokumentasi
  // di hub.cpp agar gate ini tidak di-"perbaiki" keliru saat pass berikutnya.
  auto src=read_src30("src/websocket/hub.cpp");
  EXPECT_NE(src.find("P22-H7"), std::string::npos)
    << "hub.cpp harus mendokumentasikan keputusan paritas Go: student presence "
       "via HTTP AccessLog, WS heartbeat hanya pengawas (privileged)";
}

TEST(P30, H7_ExamCompletedBlockedForNonPrivileged){
  // Gate yang sama dengan heartbeat — sebelumnya hanya heartbeat yang
  // teruji; exam_completed non-privileged bisa menghapus presence siswa
  // lain (DEL heartbeat:{exam}:{mac}).
  bool del_called=false;
  examvan::Hub hub(nullptr, [&](const std::string&){ del_called=true; }, nullptr);
  auto c=std::make_shared<examvan::Client>();
  c->room="7"; c->privileged=false;
  hub.add_client(c);
  hub.handle_message(c, examvan::marshal_socketio("exam_completed","{\"mac_address\":\"aa\"}"));
  EXPECT_FALSE(del_called)
    << "exam_completed non-privileged TIDAK boleh DEL presence siswa (phantom offline)";
}

#ifdef HAS_PROTOBUF
TEST(P30, H7_ProtoHeartbeatBlockedForNonPrivileged){
  // Jalur protobuf WsEnvelope — gate harus sama dengan jalur socket.io JSON.
  bool set_called=false;
  examvan::Hub hub([&](const std::string&,const std::string&){ set_called=true; }, nullptr, nullptr);
  auto c=std::make_shared<examvan::Client>();
  c->room="3"; c->privileged=false;
  hub.add_client(c);
  examvan::v1::Heartbeat hb;
  hb.set_mac_address("AA:BB:CC:DD:EE:FF");
  hb.set_student_name("Budi");
  std::string hb_bytes; hb.SerializeToString(&hb_bytes);
  examvan::v1::WsEnvelope env;
  env.set_event("heartbeat");
  env.set_payload(hb_bytes);
  std::string env_bytes; env.SerializeToString(&env_bytes);
  hub.handle_message(c, env_bytes);
  EXPECT_FALSE(set_called)
    << "heartbeat protobuf non-privileged juga harus diblokir (paritas gate JSON)";
}

TEST(P30, H7_ProtoHeartbeatAllowedForPrivileged){
  bool set_called=false; std::string last_key;
  examvan::Hub hub([&](const std::string& k,const std::string&){ set_called=true; last_key=k; }, nullptr, nullptr);
  auto c=std::make_shared<examvan::Client>();
  c->room="9"; c->privileged=true;
  hub.add_client(c);
  examvan::v1::Heartbeat hb;
  hb.set_mac_address("AA:BB:CC:DD:EE:FF");
  std::string hb_bytes; hb.SerializeToString(&hb_bytes);
  examvan::v1::WsEnvelope env;
  env.set_event("heartbeat");
  env.set_payload(hb_bytes);
  std::string env_bytes; env.SerializeToString(&env_bytes);
  hub.handle_message(c, env_bytes);
  EXPECT_TRUE(set_called) << "heartbeat protobuf pengawas harus diterima";
  EXPECT_NE(last_key.find("heartbeat:9:"), std::string::npos);
}
#endif

// ===== M7: scope submission di handler ==================================

TEST(P30, M7_DetailHasOwnScopeCheck){
  // submission_detail tidak boleh memercayai cek scope router — tambahkan
  // cek ownership di handler (superadmin | created_by | delegated_to),
  // paritas checkExamOwnership. Kontrak: handler punya helper scope sendiri.
  auto src=read_src30("src/handlers/admin/submissions.cpp");
  auto pos=src.find("Response submission_detail");
  ASSERT_NE(pos, std::string::npos);
  auto seg=src.substr(pos, 3000);
  EXPECT_NE(seg.find("submission_scope_ok"), std::string::npos)
    << "submission_detail harus memakai helper scope sendiri (M7)";
}

TEST(P30, M7_DeleteHasOwnScopeCheck){
  auto src=read_src30("src/handlers/admin/submissions.cpp");
  auto pos=src.find("Response delete_submission");
  ASSERT_NE(pos, std::string::npos);
  auto seg=src.substr(pos, 3000);
  EXPECT_NE(seg.find("submission_scope_ok"), std::string::npos)
    << "delete_submission harus memakai helper scope sendiri (M7)";
}

TEST(P30, M7_ScopeHelperChecksOwnerOrDelegated){
  // Helper scope: resolve exam_id milik submission → created_by/delegated_to
  // cocok dengan actor, atau superadmin. Tanpa PG (dev) fail-open seperti
  // revalidasi session (konsistensi perilaku lama).
  auto src=read_src30("src/handlers/admin/submissions.cpp");
  auto pos=src.find("submission_scope_ok");
  ASSERT_NE(pos, std::string::npos);
  auto seg=src.substr(pos, 2600);
  EXPECT_NE(seg.find("created_by"), std::string::npos)
    << "helper scope harus cek created_by exam";
  EXPECT_NE(seg.find("delegated_to"), std::string::npos)
    << "helper scope harus cek delegated_to exam";
}

// ===== M8: bulk ops hormati exam_pengawas + operator ====================

TEST(P30, M8_BulkToggleHonorsPengawasScope){
  auto src=read_src30("src/handlers/admin/exams.cpp");
  auto pos=src.find("Response bulk_toggle_exams");
  ASSERT_NE(pos, std::string::npos);
  auto seg=src.substr(pos, 4200);
  EXPECT_NE(seg.find("exam_bulk_scope_ok"), std::string::npos)
    << "bulk_toggle harus memakai helper scope yang sama dengan single-assign (M8)";
}

TEST(P30, M8_BulkDeleteHonorsPengawasScope){
  auto src=read_src30("src/handlers/admin/exams.cpp");
  auto pos=src.find("Response bulk_delete_exams");
  ASSERT_NE(pos, std::string::npos);
  auto seg=src.substr(pos, 4400);
  EXPECT_NE(seg.find("exam_bulk_scope_ok"), std::string::npos)
    << "bulk_delete harus memakai helper scope yang sama dengan single-assign (M8)";
}

TEST(P30, M8_BulkScopeChecksPengawasAndOperator){
  // Helper scope bulk: superadmin | created_by | delegated_to |
  // exam_pengawas assigned | operator same-instansi (paritas router
  // scope="exam_access" router_full.cpp:169-183).
  auto src=read_src30("src/handlers/admin/exams.cpp");
  auto pos=src.find("exam_bulk_scope_ok");
  ASSERT_NE(pos, std::string::npos);
  auto seg=src.substr(pos, 3000);
  EXPECT_NE(seg.find("exam_pengawas"), std::string::npos)
    << "scope bulk harus menghitung exam_pengawas (assigned pengawas)";
  EXPECT_NE(seg.find("instansi"), std::string::npos)
    << "scope bulk harus menghitung operator same-instansi";
}

// ===== L3: mask_token fixed-length ======================================

TEST(P30, L3_MaskTokenFixedLength){
  auto src=read_src30("src/handlers/admin/settings.cpp");
  auto pos=src.find("std::string mask_token");
  ASSERT_NE(pos, std::string::npos);
  auto seg=src.substr(pos, 700);
  EXPECT_EQ(seg.find("t.size()-4"), std::string::npos)
    << "mask tidak boleh sized (bocor panjang secret) — gunakan mask fixed-length";
  EXPECT_NE(seg.find("****"), std::string::npos)
    << "mask fixed-length (fixed asterisk) wajib ada";
}
