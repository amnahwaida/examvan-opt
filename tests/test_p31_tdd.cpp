// Pass-23 remediasi carried-over: M16-sisa, M19-sisa, B3, B4, B5 (+B6).
//
// Kontrak yang bisa diuji dari C++ (sumber/script sebagai teks) dikunci di
// sini; kontrak docker-compose/nginx murni infrastruktur dikunci oleh
// tests/test_p31_env.sh (dipanggil CI). Prinsip TDD: file ini ditulis dan
// GAGAL dulu (RED) sebelum remediasi (GREEN).
//
// M19-sisa: versi "2.7.2" hardcode sudah ditutup pass-21 (Config::version);
// sisa pass-20 adalah tooling mati — extract_contract.py path absolut.
// B3: check-docker-paths.sh serial >8 menit di CI → xargs -P (JOBS).
// B4/B5: nginx location / tidak lagi meneruskan header Upgrade; HSTS tidak
//   dikirim di port 80 (RFC 6797: header hanya sah via HTTPS).
// B6: worktree .claude dihapus + .gitignore .claude/ (terverifikasi pass-21).
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_src31(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// ===== M19-sisa: contract tooling hidup kembali ==========================

TEST(P31, M19_ExtractContractNoAbsolutePath){
  auto src=read_src31("scripts/extract_contract.py");
  ASSERT_FALSE(src.empty()) << "scripts/extract_contract.py harus ada";
  EXPECT_EQ(src.find("/home/"), std::string::npos)
    << "path absolut home-directory dilarang (mati di CI/mesin lain)";
  EXPECT_NE(src.find("--src"), std::string::npos)
    << "wajib menerima --src (path repo Go dari CLI/env CI)";
  EXPECT_NE(src.find("--out"), std::string::npos)
    << "wajib menerima --out";
}

TEST(P31, M19_ExtractContractFailsLoudOnMissingSource){
  // Gagal senyap dilarang: file sumber hilang harus exit != 0.
  int rc=std::system("python3 scripts/extract_contract.py --src /nonexistent/main.go >/dev/null 2>&1");
  if(rc<0) GTEST_SKIP() << "shell tidak tersedia";
  EXPECT_NE(rc, 0) << "source hilang harus gagal keras (exit != 0), bukan tulis contract.json kosong";
}

// ===== B3: check-docker-paths.sh paralel =================================

TEST(P31, B3_CheckDockerPathsParallel){
  auto src=read_src31("scripts/check-docker-paths.sh");
  ASSERT_FALSE(src.empty()) << "scripts/check-docker-paths.sh harus ada";
  EXPECT_NE(src.find("xargs"), std::string::npos)
    << "compile pass wajib paralel (xargs -P) — serial >8 menit rawan timeout CI";
  EXPECT_NE(src.find("JOBS"), std::string::npos)
    << "paralelisme wajib bisa diatur (JOBS) agar CI bisa membatasi";
}

// ===== B4: nginx location / tanpa header Upgrade =========================

TEST(P31, B4_LocationRootNoUpgradeHeader){
  auto src=read_src31("nginx/nginx.conf");
  ASSERT_FALSE(src.empty()) << "nginx/nginx.conf harus ada";
  auto pos=src.find("location / {");
  ASSERT_NE(pos, std::string::npos);
  auto end=src.find("\n    }", pos);
  ASSERT_NE(end, std::string::npos);
  auto seg=src.substr(pos, end-pos);
  EXPECT_EQ(seg.find("proxy_set_header Upgrade"), std::string::npos)
    << "location / TIDAK boleh meneruskan header Upgrade — WS wajib /ws/ "
       "(read_timeout 60s memutus handshake WS di path lain)";
  EXPECT_NE(seg.find("proxy_read_timeout 130s"), std::string::npos)
    << "fallback timeout 130s di location / untuk WS lama";
}

TEST(P31, B4_WsLocationKeepsUpgradeHeader){
  // Jalur /ws/ tetap butuh header upgrade — yang dilarang hanya location /.
  auto src=read_src31("nginx/nginx.conf");
  auto pos=src.find("location /ws/");
  ASSERT_NE(pos, std::string::npos) << "location /ws/ wajib tetap ada";
  auto seg=src.substr(pos, 900);
  EXPECT_NE(seg.find("proxy_set_header Upgrade"), std::string::npos)
    << "location /ws/ tetap meneruskan header Upgrade";
}

// ===== B5: HSTS tidak dikirim di port 80 =================================

TEST(P31, B5_NoHstsOnPlainHttp){
  auto src=read_src31("nginx/nginx.conf");
  ASSERT_FALSE(src.empty());
  // TLS belum aktif (listen 443 masih comment) → TIDAK boleh ada baris
  // Strict-Transport-Security aktif di mana pun (RFC 6797: diabaikan browser
  // di HTTP, menyesatkan audit).
  EXPECT_EQ(src.find("add_header Strict-Transport-Security"), std::string::npos)
    << "HSTS hanya sah via HTTPS — hapus sampai listen 443 ssl diaktifkan";
}

// ===== B6: worktree hygiene ==============================================

TEST(P31, B6_ClaudeDirIgnored){
  auto gi=read_src31(".gitignore");
  ASSERT_FALSE(gi.empty());
  EXPECT_NE(gi.find(".claude/"), std::string::npos)
    << ".claude/ wajib di .gitignore (worktree agent tidak boleh untracked lagi)";
}
