#pragma once
#include <string>
#include <vector>

namespace examvan::handlers::auth {

/* Render halaman public auth (/register, /register/confirm, /forgot-password,
 * /reset-password) dari template SOURCE templates/public/*.html — bukan file
 * rendered — karena blok {{if .email_enabled}} / {{if .turnstile_enabled}}
 * harus dievaluasi terhadap setting runtime (file rendered di-capture saat
 * semua setting mati). Urutan pemrosesan:
 *   1. {{if .X}}...{{else}}...{{end}} → sisi yang aktif (nesting-aware),
 *   2. {{range .flashes}}...{{end}} → join baris flash,
 *   3. {{ template "public_*" . }} → partial dari shared.html,
 *   4. placeholder {{.key}} → nilai (semua di-html-escape).
 * Menyisipkan CSRF cookie + token ke seluruh atribut yang relevan. */
struct PublicAuthPage {
  std::string name;            // "register" | "register_confirm" | "forgot_password" | "reset_password"
  std::string error;           // .error
  std::vector<std::string> flashes;   // .flashes
  std::string username;        // .username
  std::string email;           // .email
  std::string masked_email;    // .masked_email
  std::string form_username;   // .form_username
  std::string form_email;      // .form_email
  bool email_enabled = false;
  bool turnstile_enabled = false;
  std::string turnstile_site_key;
  std::string footer_text = "© 2026 EXAMVAN Team. All rights reserved.";
  std::string seo_description = "EXAMVAN adalah aplikasi ujian online mandiri dengan sistem keamanan tinggi terhindar dari kecurangan.";
  std::string seo_title = "EXAMVAN - Aplikasi Ujian Online Aman & Tertib";
  bool seo_index = false;
  std::string csrf_token;      // jika kosong → di-generate + Set-Cookie
};

struct RenderedAuthPage {
  std::string body;
  std::string set_cookie;      // "csrf_token=...; Path=/; SameSite=Lax; ..."
};

RenderedAuthPage render_auth_page(const PublicAuthPage& page);

} // namespace examvan::handlers::auth