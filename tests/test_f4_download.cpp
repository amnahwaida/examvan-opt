#include <gtest/gtest.h>
#include "handlers/public/download.hpp"
#include "handlers/r2/r2.hpp"
using namespace examvan::handlers::public_;
using namespace examvan::r2;

TEST(F4Download, Apk302Presign) {
  R2Config cfg{"k","s","https://ep","b"};
  auto url=presign_url(cfg, object_key_for_app("2.7.2","student"), 3600);
  EXPECT_NE(url.find("b/apps/android/2.7.2"), std::string::npos);
  EXPECT_NE(url.find("X-Amz-Signature"), std::string::npos);
}

TEST(F4Download, PageHtml) {
  examvan::Request req;
  auto res=download_page(req);
  EXPECT_EQ(res.status,200);
  EXPECT_NE(res.body.find("download-card"), std::string::npos);
}

TEST(F4Download, ApkRedirect) {
  examvan::Request req;
  auto res=download_apk(req);
  EXPECT_EQ(res.status,302);
  EXPECT_FALSE(res.headers["Location"].empty());
}

TEST(F4Download, SystemAppNeedsId) {
  examvan::Request req;
  EXPECT_EQ(download_system_app(req).status,404);
  req.params["id"]="42";
  auto res=download_system_app(req);
  EXPECT_EQ(res.status,302);
  EXPECT_NE(res.headers["Location"].find("42"), std::string::npos);
}

TEST(F4Download, R2NotConfigured) {
  R2Config cfg{"","","",""};
  EXPECT_FALSE(cfg.enabled());
  EXPECT_EQ(presign_url(cfg,"key.pdf"), "");
}
