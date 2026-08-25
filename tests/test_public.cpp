#include <gtest/gtest.h>
#include "handlers/public/hasil.hpp"
#include "handlers/public/download.hpp"
using namespace examvan::handlers::public_;

TEST(Public, CekHasilPage) {
  examvan::Request req;
  EXPECT_EQ(cek_hasil_page(req).status,200);
  EXPECT_NE(cek_hasil_page(req).body.find("Cek Hasil"), std::string::npos);
}

TEST(Public, HasilPage404NoToken) {
  examvan::Request req;
  EXPECT_EQ(hasil_page(req).status,404);
  req.params["token"]="abc123";
  EXPECT_EQ(hasil_page(req).status,200);
}

TEST(Public, DownloadApkRedirect) {
  examvan::Request req;
  EXPECT_EQ(download_apk(req).status,302);
}

TEST(Public, DownloadSystemAppNeedsId) {
  examvan::Request req;
  EXPECT_EQ(download_system_app(req).status,404);
  req.params["id"]="42";
  EXPECT_EQ(download_system_app(req).status,302);
}
