#include <gtest/gtest.h>
#include "handlers/admin/exams.hpp"
#include "middleware/protobuf.hpp"
#include "config/config.hpp"
#include <cstdlib>
using namespace examvan;

/*
 * TDD Protobuf Mandatory — dokumentasi kontrak performa 2c/8GB
 *
 * Latar: JSON 30-50% lebih boros di hot path heartbeat (500k msg/menit) dan
 * exam creation. Protobuf mandatory mengurangi 40% byte, 60% CPU, 35% RSS
 * sesuai docs/ARCHITECTURE_CPP_UWS_PROTOBUF.md §2.
 *
 * Kontrak:
 * - PROTOBUF_MANDATORY=0 (development): terima JSON + Protobuf, log WARN
 * - PROTOBUF_MANDATORY=1 (production): POST/PUT/PATCH dengan JSON -> 415
 *   PROTOBUF_REQUIRED, Accept: application/json saja -> 406
 * - multipart/form-data dikecualikan (file upload tetap FormData)
 * - Content-Type: application/x-protobuf atau application/vnd.examvan.v1+protobuf -> diterima
 */

TEST(ProtobufMandatory, JsonRejectedWhenMandatory){
  setenv("PROTOBUF_MANDATORY","1",1);
  setenv("APP_ENV","production",1);
  Config cfg=Config::load();
  ASSERT_TRUE(cfg.protobuf_mandatory);
  Request req;
  req.method="POST";
  req.headers["Content-Type"]="application/json";
  req.body="{\"name\":\"Ujian\",\"file_path\":\"/tmp/a.pdf\"}";
  auto err=middleware::require_protobuf(req,cfg);
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->status,415);
  EXPECT_NE(err->body.find("PROTOBUF_REQUIRED"), std::string::npos);
  setenv("PROTOBUF_MANDATORY","0",1);
  setenv("APP_ENV","development",1);
}

TEST(ProtobufMandatory, ProtobufAcceptedWhenMandatory){
  setenv("PROTOBUF_MANDATORY","1",1);
  setenv("APP_ENV","production",1);
  Config cfg=Config::load();
  Request req;
  req.method="POST";
  req.headers["Content-Type"]="application/x-protobuf";
  req.body="fake-protobuf-bytes"; // body non-kosong tapi content is protobuf -> tidak ditolak di middleware level (hanya check header)
  auto err=middleware::require_protobuf(req,cfg);
  EXPECT_FALSE(err.has_value()) << "protobuf content harus diterima";
  setenv("PROTOBUF_MANDATORY","0",1);
}

TEST(ProtobufMandatory, MultipartExemptEvenWhenMandatory){
  setenv("PROTOBUF_MANDATORY","1",1);
  setenv("APP_ENV","production",1);
  Config cfg=Config::load();
  Request req;
  req.method="POST";
  req.headers["Content-Type"]="multipart/form-data; boundary=----123";
  req.body="--123\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nUjian\r\n--123--";
  // Untuk file upload, meskipun mandatory, multipart dikecualikan di handler level
  // Di middleware, multipart tetap akan ditolak? Kita buat pengecualian di handler, bukan middleware
  // Test ini kunci bahwa handler create_exam tetap terima multipart meski mandatory
  auto res=handlers::admin::create_exam(req);
  // Multipart tanpa file R2 tidak perlu, tapi minimal tidak 415
  EXPECT_NE(res.status,415) << res.body;
  setenv("PROTOBUF_MANDATORY","0",1);
}

TEST(ProtobufMandatory, ConfigDefaults){
  unsetenv("PROTOBUF_MANDATORY");
  setenv("APP_ENV","development",1);
  Config c1=Config::load();
  EXPECT_FALSE(c1.protobuf_mandatory) << "development default 0";
  setenv("APP_ENV","production",1);
  // tanpa env, default sekarang 0 (opt-in), bukan 1, untuk migrasi bertahap
  Config c2=Config::load();
  EXPECT_FALSE(c2.protobuf_mandatory) << "production tanpa env default 0 (opt-in)";
  setenv("PROTOBUF_MANDATORY","1",1);
  Config c3=Config::load();
  EXPECT_TRUE(c3.protobuf_mandatory);
  setenv("PROTOBUF_MANDATORY","0",1);
}

TEST(ProtobufMandatory, IsProtobufContentDetection){
  Request r1; r1.headers["Content-Type"]="application/x-protobuf";
  EXPECT_TRUE(middleware::is_protobuf_content(r1));
  Request r2; r2.headers["Content-Type"]="application/vnd.examvan.v1+protobuf";
  EXPECT_TRUE(middleware::is_protobuf_content(r2));
  Request r3; r3.headers["Content-Type"]="application/json";
  EXPECT_FALSE(middleware::is_protobuf_content(r3));
  Request r4; r4.headers["content-type"]="APPLICATION/X-PROTOBUF"; // case-insensitive
  EXPECT_TRUE(middleware::is_protobuf_content(r4));
}
