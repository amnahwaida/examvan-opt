#pragma once
#include <string>
namespace examvan::handlers::admin {
std::string render_admin_template(const std::string& name, const std::string& version);

/* C5: render halaman admin + suntik token CSRF segar ke <meta
 * name="csrf-token"> dan semua input hidden csrf_token/_csrf, lalu
 * kembalikan (html, cookie_header). Handler halaman memakai ini supaya token
 * yang dikirim frontend (X-CSRF-Token dari meta) COCOK dengan cookie yang
 * diverifikasi admin_api. Tanpa ini token meta hardcoded di
 * *.rendered.html ≠ cookie sesi → verifikasi CSRF akan memblokir UI. */
struct RenderedAdminPage { std::string html; std::string csrf_cookie; };
RenderedAdminPage render_admin_page(const std::string& name, const std::string& version);
}
