#pragma once
#include "models/exam.hpp"
#include <string>
#include <optional>
#include <vector>
#include <functional>

namespace examvan::store {

/*
 * Abstraction layer untuk penyimpanan exam.
 *
 * Tujuan: memungkinkan swap implementasi tanpa mengubah handler.
 * Saat ini hanya ada implementasi in-memory (exam_store_memory), tapi
 * interface ini disiapkan agar implementasi PostgreSQL (HAS_LIBPQ)
 * bisa di-drop-in di fase berikutnya.
 *
 * Semua method thread-safe (implementasi wajib menjamin konsistensi
 * terhadap akses bersamaan).
 */
class ExamStore {
public:
  virtual ~ExamStore() = default;

  // Ambil id berikutnya (monotonic, tidak pernah kembali).
  virtual int next_id() = 0;

  // Tambahkan exam ke store. Mengembalikan false jika token sudah
  // dipakai (collision). Operasi atomic: cek + insert dalam satu lock.
  // ID exam tidak di-set oleh add() — handler harus set exam.id = next_id().
  virtual bool add(const models::Exam& e) = 0;

  virtual std::optional<models::Exam> get_by_id(int id) = 0;

  // Kembalikan snapshot seluruh exam.
  virtual std::vector<models::Exam> list_all() = 0;

  // Cek apakah token sudah dipakai exam selain exclude_id (exclude_id<=0 = cek semua).
  virtual bool token_exists(const std::string& token, int exclude_id = 0) = 0;

  // Cek token ke dalam kumpulan token yang sudah pernah dikeluarkan (auto-gen).
  // Juga cek terhadap exams_[] token yang sudah ada (termasuk custom token).
  // Mengembalikan false jika sudah ada (collision).
  virtual bool claim_token(const std::string& token) = 0;

  // Lepaskan token dari seen_tokens_ (dipanggil setelah claim_token sukses
  // namun operasi berikutnya gagal).
  virtual void unclaim_token(const std::string& token) = 0;

  // Mutasi exam dengan id tertentu di dalam lock store. Mengembalikan
  // false jika id tidak ditemukan.
  virtual bool update(int id, const std::function<void(models::Exam&)>& mutator) = 0;

  // Hapus exam dengan id tertentu. Mengembalikan false jika tidak ada.
  virtual bool remove(int id) = 0;

  // Jumlah exam tersimpan.
  virtual size_t count() = 0;

  // Reset state untuk keperluan test (kosongkan semua data + counter).
  virtual void clear_all() = 0;
};

// Singleton implementasi in-memory (default). Handler memanggil ini.
ExamStore* memory_store();

// Ganti store aktif (untuk test / swap ke PG di fase berikutnya).
void set_active_store(ExamStore* store);

// Store aktif; default = memory_store().
ExamStore* active_store();

} // namespace examvan::store
