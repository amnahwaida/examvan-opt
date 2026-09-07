#pragma once
#include "db/pool.hpp"

#ifdef HAS_LIBPQ
#include "db/pool_real.hpp"
#include <mutex>
#include <string>

namespace examvan::db {

/* P21-T2: pool PG proses-wide — SATU RealPool hidup sepanjang umur proses.
 * Sebelumnya pola `with_pg` di tiap handler membangun RealPool stack-lokal
 * per request; koneksi di-release ke idle_ pool lokal lalu dtor men-PQfinish
 * semuanya → TCP+auth PG baru pada setiap request (churn ribuan koneksi/menit
 * di bawah polling admin + heartbeat flusher).
 *
 * RealPool::acquire() membuka koneksi baru on-demand bila idle kosong, dan
 * release() mengembalikannya ke idle_ — dengan pool proses-wide koneksi
 * benar-benar di-reuse lintas request. Mutex internal RealPool menjaga
 * thread-safety antar worker uWS/posix.
 *
 * Inisialisasi lazy: panggilan pertama membaca conninfo dari Config/env.
 * Bila DATABASE_URL tidak di-set (mode uji/memori), kembalikan nullptr —
 * caller memperlakukan sama seperti "PG tidak tersedia". */
inline RealPool* global_pool(){
  static RealPool pool;
  static std::once_flag once;
  std::call_once(once, [](){
    auto cfg=Config::load();
    std::string url=cfg.database_url;
    if(url.empty()) if(auto* e=getenv("DATABASE_URL")) url=e;
    if(url.empty()) return;              // mode memori/dev — pool dibiarkan kosong
    std::string ci=conninfo_from_url_or_raw(url);
    if(ci.empty()) ci=url;
    pool=RealPool(ci, cfg.database_max_conns);
  });
  // Pool kosong (tanpa conninfo) → null; caller jangan mencoba acquire.
  static const bool configured=[]{
    auto cfg=Config::load();
    std::string url=cfg.database_url;
    if(url.empty()) if(auto* e=getenv("DATABASE_URL")) url=e;
    return !url.empty();
  }();
  return configured? &pool : nullptr;
}

/* Pola with_pg proses-wide (pengganti 5 duplikat stack-lokal di handler):
 * fn dipanggil HANYA bila PG terkonfigurasi dan satu koneksi hidup berhasil
 * diambil; koneksi dikembalikan ke pool (bukan di-close) setelah fn selesai.
 * Catatan: fn menerima RealPool& supaya transaksi multi-statement tetap bisa
 * memegang satu koneksi (acquire → BEGIN/…/COMMIT → release). */
template <typename Fn>
inline bool with_global_pg(Fn&& fn){
  RealPool* p=global_pool();
  if(!p) return false;
  auto c=p->acquire();
  if(!c || PQstatus(c.get())!=CONNECTION_OK){
    // Koneksi rusak → buang; coba sekali lagi (acquire membuka baru bila
    // idle kosong; pool memanggil PQfinish pada conn bermasalah via dtor).
    c=p->acquire();
    if(!c || PQstatus(c.get())!=CONNECTION_OK) return false;
  }
  fn(*p);
  p->release(c.release());
  return true;
}

} // namespace examvan::db
#endif
