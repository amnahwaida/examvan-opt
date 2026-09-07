// Pass-20 C5-parsial: POSIX worker + acceptor masih detach() — graceful
// shutdown hanya menutup listen fd, thread hidup sampai proses mati (tidak
// bisa di-join oleh Server::stop(), beda dengan jalur uWS). Kontrak:
// 1) TIDAK ADA `.detach()` di server.cpp (uWS maupun posix).
// 2) Worker posix disimpan sebagai member joinable (vector<std::thread>).
// 3) stop() posix menutup listen fd + notify CV + join semua thread.
// 4) main.cpp memanggil srv.stop() di jalur shutdown (SIGTERM/SIGINT).
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_src27(const std::string& p){
  std::ifstream f(p);
  if(!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

TEST(P27, NoDetachAnywhereInServer){
  // C5: detach() menghilangkan kemampuan join — graceful shutdown mustahil
  // di jalur posix. Dilarang di seluruh server.cpp (uWS maupun posix).
  auto src = read_src27("src/server/server.cpp");
  EXPECT_EQ(src.find(".detach()"), std::string::npos)
    << "server.cpp tidak boleh memakai .detach() — simpan thread sebagai member joinable";
}

TEST(P27, PosixThreadsStoredAsJoinableMembers){
  // Worker/acceptor posix wajib disimpan (vector<std::thread>) supaya stop()
  // bisa join — bukan thread sementara yang langsung detached.
  auto src = read_src27("src/server/server.cpp");
  EXPECT_NE(src.find("posix_threads_"), std::string::npos)
    << "butuh member posix_threads_ (vector<std::thread>) di Server";
  auto hpp = read_src27("src/server/server.hpp");
  EXPECT_NE(hpp.find("posix_threads_"), std::string::npos)
    << "deklarasi posix_threads_ harus ada di server.hpp";
  EXPECT_NE(hpp.find("#include <thread>"), std::string::npos)
    << "server.hpp harus include <thread> untuk vector<std::thread>";
}

TEST(P27, PosixStopNotifiesAndJoins){
  // stop() posix: notify CV agar worker keluar dari wait, lalu join semua
  // thread (worker + acceptor). Tanpa notify, join bisa menggantung.
  auto src = read_src27("src/server/server.cpp");
  auto pos = src.find("void Server::stop()");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 3000);
  EXPECT_NE(seg.find("notify_all"), std::string::npos)
    << "stop() harus notify_all CV posix agar worker keluar dari wait";
  EXPECT_NE(seg.find("posix_threads_"), std::string::npos)
    << "stop() harus join posix_threads_";
  EXPECT_NE(seg.find("join()"), std::string::npos)
    << "stop() harus memanggil join() di jalur posix";
}

TEST(P27, MainCallsServerStopOnShutdown){
  // P18-C5 sudah men-stops worker/flusher/jobs di main.cpp, tapi srv.stop()
  // tidak pernah dipanggil — uWS thread tidak pernah di-join, posix fd tak
  // ditutup rapi. Wajib ada di jalur shutdown.
  auto src = read_src27("src/main.cpp");
  EXPECT_NE(src.find("srv.stop()"), std::string::npos)
    << "main.cpp harus memanggil srv.stop() pada shutdown (sebelum/sesudah drain queue)";
}

TEST(P27, PosixWorkersDrainRemainingQueue){
  // Worker posix tidak boleh langsung break saat g_running false bila masih
  // ada koneksi antre — harus drain (handle koneksi yang sudah diterima).
  auto src = read_src27("src/server/server.cpp");
  auto pos = src.find("posix_worker_loop");
  ASSERT_NE(pos, std::string::npos);
  auto seg = src.substr(pos, 2000);
  EXPECT_NE(seg.find("!g_posix_q->empty()"), std::string::npos)
    << "worker harus lanjut memproses koneksi yang sudah masuk antre saat shutdown (drain)";
}
