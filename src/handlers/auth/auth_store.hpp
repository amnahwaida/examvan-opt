#pragma once
#include <optional>
#include <string>
#include <unordered_map>
#include <mutex>

namespace examvan::handlers::auth {

/* Penyimpanan user pendaftaran public. Dua mode:
 *  - produksi: SELECT/INSERT/UPDATE/DELETE ke tabel PG `admin_users` (via
 *    DATABASE_URL) — paritas Go models.CreateUser / GetUserByUsername dll;
 *  - unit test: peta in-memory (EXAMVAN_TESTING / hook set_registered_user_for_test),
 *    sehingga suite berjalan tanpa database.
 * Semua method memakai mutex — aman dipanggil dari thread uWS mana pun. */

struct RegisteredUser {
  int id = 0;
  std::string username;
  std::string email;
  std::string password_hash;
  std::string status;            // active | pending_otp
  std::string otp_code;
  int otp_attempts = 0;
  long otp_expiry_epoch = 0;     // detik UTC epoch; 0 = tidak ada
  // Kuota default dari saas_settings (diisi handler saat INSERT).
  int max_exams = 3;
  long max_pdf_size = 1048576;
  int max_concurrent_exams = 2;
  long max_storage_size = 52428800;
  int active_days = 14;
  std::string registered_ip;
};

bool find_registered_user(const std::string& username, RegisteredUser& out);
bool find_registered_user_by_email(const std::string& email, RegisteredUser& out);
int count_recent_registrations_by_ip(const std::string& ip);

// INSERT akun baru. Mengembalikan false bila gagal (PG error / username or
// email sudah dipakai).
bool insert_registered_user(const RegisteredUser& u);

// UPDATE kolom OTP (resend/forgot) — menulis kode + expiry baru, reset attempts.
bool update_user_otp(const std::string& username, const std::string& otp_code, long otp_expiry_epoch);

// UPDATE status aktif + bersihkan OTP (register/confirm sukses).
bool activate_registered_user(const std::string& username);

// UPDATE otp_attempts (+1) — salah tebak OTP. Mengembalikan JUMLAH percobaan
// BARU setelah increment (atomik; M6 — cegah race confirm/resend yang membaca
// nilai lama). -1 bila user tidak ditemukan / gagal.
int bump_otp_attempts(const std::string& username);

// UPDATE password_hash + bersihkan OTP (reset sukses).
bool update_user_password(const std::string& username, const std::string& password_hash);

// DELETE baris (register/confirm gagal permanen / kedaluwarsa).
bool delete_registered_user(const std::string& username);

// Ambil saas_settings dari PG (fallback default bila PG tidak ada).
std::string get_setting(const std::string& key, const std::string& def);

} // namespace examvan::handlers::auth