# Alur Halaman Public — EXAMVAN C++ (examvan-opt)

Dokumen ini merangkum alur halaman public (tanpa autentikasi admin) pada
implementasi C++ (`examvan-opt`), serta status paritas terhadap referensi Go
frozen (`EXAMVAN/webui`). Berlaku untuk versi 2.7.x.

## Peta Endpoint Public

| Metode | Path | Handler (C++) | Status |
|---|---|---|---|
| GET | `/` | `register_routes` → index | ✅ live |
| GET | `/login` · `/admin/login` | `auth::login_page` | ✅ live |
| POST | `/login` · `/admin/login` | `auth::login_handler` | ✅ live |
| POST | `/logout` | `auth::logout_handler` | ✅ live |
| GET | `/logout` | `auth::logout_page` (redirect) | ✅ live |
| GET | `/register` | `auth::register_page` | ✅ live (ini) |
| POST | `/register` | `auth::register_handler` | ✅ live (ini) |
| GET | `/register/confirm?username=` | `auth::register_confirm_page` | ✅ live (ini) |
| POST | `/register/confirm?username=` | `auth::register_confirm_handler` | ✅ live (ini) |
| POST | `/register/resend?username=` | `auth::resend_otp` | ✅ live (ini) |
| GET | `/forgot-password` | `auth::forgot_password_page` | ✅ live (ini) |
| POST | `/forgot-password` | `auth::forgot_password_handler` | ✅ live (ini) |
| GET | `/reset-password?username=` | `auth::reset_password_page` | ✅ live (ini) |
| POST | `/reset-password?username=` | `auth::reset_password_handler` | ✅ live (ini) |
| GET | `/download` · `/download/apk` · `/download/app/:id` | `public_::download_*` | ✅ live |
| GET | `/hasil` | `public_::cek_hasil_page` | ✅ live |
| GET | `/hasil/:token` | `public_::hasil_page` | ✅ live |
| GET | `/:token` | 302 → `/hasil/:token` | ✅ live |
| GET | `/api/hasil/:token` | `public_::cek_hasil_api` | ✅ live + rate limit 30/mnt |

## Alur Pendaftaran (`/register`)

```mermaid
flowchart TD
  A[GET /register] --> B{Ada sesi login?}
  B -- ya --> D[302 /admin/dashboard]
  B -- tidak --> F[Form: username, email, password, CSRF, Turnstile?]
  F --> G[POST /register]
  G --> H{Validasi:<br/>kosong? username regex?<br/>pass>=8? email @/.?}
  H -- gagal --> F[re-render + .error + echo username/email]
  H -- ok --> I{Limit per-IP 24 jam<br/>saas max_accounts_per_ip}
  I -- penuh --> F
  I -- ok --> J{Turnstile aktif?}
  J -- gagal --> F
  J -- ok --> K{Email domain whitelist}
  K -- ditolak --> F
  K -- ok --> L{Username unik? Email unik?}
  L -- bentrok --> F
  L -- ok --> M[INSERT admin_users<br/>status=active|pending_otp,<br/>role=['guru'], instansi=personal,<br/>kuota dari saas_settings]
  M --> N{email_verification_enabled?}
  N -- ya --> O[OTP 6 digit + expiry 15m<br/>kirim email SMTP]
  O -- gagal kirim --> P[DELETE user + error]
  O -- sukses --> Q[302 /register/confirm?username=]
  N -- tidak --> R[flash + 302 /login]
```

Catatan penting:

- **Respons netral**: endpoint resend/forgot tidak pernah membocorkan
  keberadaan akun — semua cabang mengembalikan pesan seragam.
- **Anti brute force**: OTP salah maks 5× → OTP dihapus (register) / dinonaktifkan
  (reset); OTP kedaluwarsa 15 menit; resend cooldown 60 detik.
- **Fail-closed**: Turnstile aktif tanpa token valid → tolak; SMTP tak
  terkofigurasi → flow netral tetap jalan, email tidak dikirim.
- **Password tidak pernah di-echo** saat validasi gagal.

## Alur Konfirmasi Email (`/register/confirm`)

1. `GET /register/confirm?username=X` — halaman **selalu dirender (200)**,
   netral terhadap status user (anti enumerasi): email termasking hanya
   diisi bila user benar-benar `pending_otp`.
2. `POST /register/confirm?username=X` (CSRF + rate limit 5/menit):
   - kode salah → `otp_attempts+1`; pada percobaan ke-5 OTP **dinonaktifkan**
     (bukan hapus akun — mencegah CSRF-DoS lewat POST 5× OTP salah);
   - kode kedaluwarsa (> 15 menit) → user **dihapus** (anti penimbunan
     akun pending);
   - kode benar → `status=active`, `otp_code=NULL`, flash, 302 `/login`.
3. `POST /register/resend?username=X` (CSRF, JSON): rotasi OTP baru bila
   status masih `pending_otp` dan di luar cooldown 60 detik; respons seragam
   untuk semua cabang (anti enumerasi).

## Alur Lupa Password (`/forgot-password` → `/reset-password`)

```mermaid
flowchart TD
  A[GET /forgot-password] --> B[POST /forgot-password]
  B --> C{username kosong?}
  C -- ya --> B
  C -- tidak --> D{Turnstile aktif?}
  D -- gagal --> B
  D -- ok --> E{user aktif + email + SMTP?}
  E -- ya, di luar cooldown --> F[UPDATE otp_code/expiry + kirim email]
  E -- tidak --> G[lewat — netral]
  G --> H[302 /reset-password?username=]
  F --> H
  H --> I[GET /reset-password?username=]
  I --> J{username kosong?}
  J -- ya --> 302 /forgot-password
  J -- tidak --> K[POST /reset-password]
  K --> L{OTP+pass wajib, pass>=8,<br/>konfirmasi cocok, Turnstile}
  L -- gagal --> K
  L -- ok --> M{OTP valid & belum expired?}
  M -- salah --> N[attempt+1; di 5x OTP dihapus]<br/>pesan seragam
  M -- expired --> O[pesan kedaluwarsa]
  M -- benar --> P[UPDATE password_hash + bersihkan OTP<br/>flash + 302 /login]
```

Pesan seragam "Kode OTP salah atau sudah tidak berlaku" untuk: user tak
ditemukan, status pending, OTP sudah dipakai/dinonaktifkan, dan tebakan salah
— sehingga attacker tidak bisa membedakan akun yang benar-benar punya kode
reset.

## Keamanan Lintas Endpoint

- **CSRF**: semua mutasi (POST login/register/confirm/resend/forgot/reset)
  wajib `csrf_token` (cookie `csrf_token` + form/header) — `verify_csrf`.
- **Turnstile**: `middleware::verify_turnstile` fail-closed; token bypass hanya
  di dev; setting `turnstile_enabled` dari `saas_settings`.
- **Rate limit**: per-IP via header `X-Real-IP`/`X-Forwarded-For` (di-forward
  uWS di `server.cpp`); login 10/mnt, register/confirm/resend/forgot/reset
  5/mnt, `/api/hasil/:token` 30/mnt.
- **Session**: cookie `examvan_session` HMAC-SHA256 + rotasi kunci ganda,
  HttpOnly + SameSite=Lax (+Secure di prod), payload berisi `admin_id` dan
  `role` asli dari DB.
- **Enumeration**: respons netral untuk forgot/resend; username cadangan
  (`admin`, `superadmin`) dilarang untuk registrasi public.

## Template Rendering

Halaman public memakai `templates/public/*.rendered.html` (hasil render Go
yang di-capture) dengan substitusi placeholder string di C++
(`render_public_template` + renderer konteks `auth`): `.version`, `.csrf_token`,
`.error`, `.flashes`, `.username`, `.email`, `.masked_email`,
`.form_username`, `.form_email`, `.email_enabled`, `.turnstile_enabled`,
`.turnstile_site_key`, `.footer_text`, `.seo_*`. Partial `public_head`,
`public_nav`/`public_auth_nav`, `public_foot`, `public_skip_link` dari
`shared.html`. Fallback ke `.html` mentah bila file rendered tidak ada.

Catatan legacy: dua file rendered pernah ter-capture keliru
(`register_confirm.rendered.html` berisi halaman login admin,
`reset-password.rendered.html` berisi halaman forgot) — implementasi memakai
nama underscore dari source template dan kembali ke `.html` bila diperlukan.