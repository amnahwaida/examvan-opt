#include "handlers/auth/recovery.hpp"
#include "handlers/auth/auth_store.hpp"
#include "handlers/auth/auth_helpers.hpp"
#include "handlers/auth/template_renderer.hpp"
#include "session/csrf.hpp"
#include "session/cookie.hpp"
#include "middleware/turnstile.hpp"
#include "middleware/ratelimit.hpp"
#include "helpers/utils.hpp"
#include "helpers/password.hpp"
#include "helpers/smtp.hpp"
#include "config/config.hpp"
#include <cctype>
#include <ctime>
#include <random>
#include <openssl/rand.h>

namespace examvan::handlers::auth {

namespace {

constexpr int kMaxOtpAttempts = 5;
constexpr long kOtpTtlSec = 15 * 60;
constexpr long kResendCooldownSec = 60;

std::string gen_otp(){
  unsigned char b[6];
  std::string s;
  if(RAND_bytes(b, sizeof(b))==1){
    for(unsigned char x: b) s.push_back(char('0'+(x%10)));
  } else {
    std::random_device rd; std::mt19937 g(rd());
    for(int i=0;i<6;i++) s.push_back(char('0'+(g()%10)));
  }
  return s;
}

long now_epoch(){ return static_cast<long>(std::time(nullptr)); }

std::string get_setting_str(const char* k, const std::string& def){ return get_setting(k, def); }

// Render halaman error reset/confirm + KEMBALIKAN cookie CSRF-nya juga —
// tanpa Set-Cookie, token di hidden input tak cocok dengan cookie lama dan
// percobaan ulang akan gagal verifikasi CSRF (double-submit).
RenderedAuthPage page_register_confirm_like(const std::string& name, const std::string& username,
                                            const std::string& error, const std::string& masked){
  PublicAuthPage p; p.name=name; p.username=username; p.error=error;
  p.masked_email=masked;
  p.email_enabled=true;
  p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
  p.turnstile_site_key=get_setting_str("turnstile_site_key","");
  return render_auth_page(p);
}

// Cek apakah ada sesi login aktif → sudah login tidak boleh lihat forgot/reset.
bool has_session(const Request& req){
  std::string cookie_hdr=get_hdr_ci(req,"Cookie");
  if(cookie_hdr.empty()) return false;
  auto cfg=Config::load();
  std::string s=cfg.secret_key, prev=cfg.secret_prev;
  auto sess=prev.empty()? verify_session_cookie(s, cookie_hdr) : verify_session_cookie_dual(s, prev, cookie_hdr);
  return sess.has_value();
}

} // namespace

// ========================= GET /forgot-password =========================
Response forgot_password_page(const Request& req){
  if(has_session(req)){ Response r; r.status=302; r.headers["Location"]="/admin/dashboard"; return r; }
  PublicAuthPage p; p.name="forgot_password";
  p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
  p.turnstile_site_key=get_setting_str("turnstile_site_key","");
  RenderedAuthPage rp=render_auth_page(p);
  Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
  r.body=rp.body.empty()? "<html>Forgot</html>" : rp.body;
  return r;
}

// ========================= POST /forgot-password =========================
Response forgot_password_handler(const Request& req, const Config& cfg){
  auto form=helpers::parse_form(req.body);
  if(!csrf_ok(req, form)){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}"); return r;
  }
  std::string username=form.count("username")? form["username"]:"";
  for(char &c: username) c=tolower((unsigned char)c);
  username=helpers::sanitize_student_input(username);
  if(username.empty()){
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Username wajib diisi.\"}"); return r; }
    PublicAuthPage p; p.name="forgot_password"; p.error="Username wajib diisi.";
    p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
    p.turnstile_site_key=get_setting_str("turnstile_site_key","");
    RenderedAuthPage rp=render_auth_page(p);
    Response r; r.status=400; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }
  std::string ip=client_ip(req);
  static middleware::RateLimiter g_forgot_rl(5, std::chrono::minutes(1));
  if(!g_forgot_rl.allow("forgot:"+ip)){
    if(wants_json(req)){ Response r; r.json(429, "{\"error\":\"Terlalu banyak permintaan. Coba lagi nanti.\"}"); return r; }
    PublicAuthPage p; p.name="forgot_password"; p.error="Terlalu banyak permintaan. Coba lagi nanti.";
    p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
    p.turnstile_site_key=get_setting_str("turnstile_site_key","");
    RenderedAuthPage rp=render_auth_page(p);
    Response r; r.status=429; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }
  bool ts_enabled=get_setting_str("turnstile_enabled","0")=="1";
  if(ts_enabled){
    std::string token=form.count("cf-turnstile-response")? form["cf-turnstile-response"]:"";
    std::string secret=cfg.turnstile_secret.empty()? get_setting_str("turnstile_secret_key","") : cfg.turnstile_secret;
    if(!middleware::verify_turnstile(token, secret, ip)){
      if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Verifikasi keamanan gagal. Coba lagi.\"}"); return r; }
      PublicAuthPage p; p.name="forgot_password"; p.error="Verifikasi keamanan gagal. Coba lagi.";
      p.turnstile_enabled=true; p.turnstile_site_key=get_setting_str("turnstile_site_key","");
      RenderedAuthPage rp=render_auth_page(p);
      Response r; r.status=400; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
      r.body=rp.body; return r;
    }
  }
  RegisteredUser u;
  bool mail_sent=false;
  long now=now_epoch();
  if(find_registered_user(username, u) && u.status=="active" && !u.email.empty()){
    long last = u.otp_expiry_epoch - kOtpTtlSec;
    if(u.otp_code.empty() || u.otp_expiry_epoch<=0 || now >= last + kResendCooldownSec){
      std::string new_otp=gen_otp();
      update_user_otp(username, new_otp, now+kOtpTtlSec);
      std::string host=get_setting_str("smtp_host","smtp.gmail.com");
      std::string port=get_setting_str("smtp_port","587");
      std::string user=get_setting_str("smtp_user","");
      std::string pass=get_setting_str("smtp_password","");
      std::string sender=get_setting_str("smtp_sender_name","EXAMVAN");
      std::string err=helpers::send_password_reset_email(host, port, user, pass, sender, u.email, u.username, new_otp);
      mail_sent=err.empty();
    }
  }
  // Netral: tidak membocorkan apakah username terdaftar — semua cabang → reset page.
  (void)mail_sent;
  return success_response(wants_json(req), "/reset-password?username="+url_encode(username),
                          "{\"success\":true,\"next\":\"/reset-password?username="+url_encode(username)+"\"}");
}

// ========================= GET /reset-password =========================
Response reset_password_page(const Request& req){
  if(has_session(req)){ Response r; r.status=302; r.headers["Location"]="/admin/dashboard"; return r; }
  auto q=helpers::parse_form(req.query);
  std::string username=q.count("username")? q["username"]:"";
  if(username.empty()){
    Response r; r.status=302; r.headers["Location"]="/forgot-password"; return r;
  }
  PublicAuthPage p; p.name="reset_password"; p.username=username;
  p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
  p.turnstile_site_key=get_setting_str("turnstile_site_key","");
  RenderedAuthPage rp=render_auth_page(p);
  Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
  r.body=rp.body.empty()? "<html>Reset</html>" : rp.body;
  return r;
}

// ========================= POST /reset-password =========================
Response reset_password_handler(const Request& req, const Config& cfg){
  auto q=helpers::parse_form(req.query);
  std::string username=q.count("username")? q["username"]:"";
  auto form=helpers::parse_form(req.body);
  if(username.empty() || !csrf_ok(req, form)){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}"); return r;
  }
  std::string ip=client_ip(req);
  static middleware::RateLimiter g_reset_rl(5, std::chrono::minutes(1));
  if(!g_reset_rl.allow("reset:"+ip)){
    if(wants_json(req)){ Response r; r.json(429, "{\"error\":\"Terlalu banyak percobaan. Coba lagi nanti.\"}"); return r; }
    RenderedAuthPage rp=page_register_confirm_like("reset_password", username, "Terlalu banyak percobaan. Coba lagi nanti.", "");
    Response r; r.status=429; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }
  std::string otp=form.count("otp_code")? form["otp_code"]:"";
  std::string password=form.count("password")? form["password"]:"";
  std::string password_confirm=form.count("password_confirm")? form["password_confirm"]:"";
  std::string err;
  if(otp.empty() || password.empty() || password_confirm.empty()) err="OTP dan password baru wajib diisi.";
  else if(password.size()<8) err="Password minimal 8 karakter.";
  else if(password.size()>72) err="Password maksimal 72 karakter (batas bcrypt).";
  else if(password!=password_confirm) err="Konfirmasi password tidak cocok.";
  bool ts_enabled=get_setting_str("turnstile_enabled","0")=="1";
  if(err.empty() && ts_enabled){
    std::string token=form.count("cf-turnstile-response")? form["cf-turnstile-response"]:"";
    std::string secret=cfg.turnstile_secret.empty()? get_setting_str("turnstile_secret_key","") : cfg.turnstile_secret;
    if(!middleware::verify_turnstile(token, secret, ip)) err="Verifikasi keamanan gagal. Coba lagi.";
  }
  if(!err.empty()){
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\""+err+"\"}"); return r; }
    RenderedAuthPage rp=page_register_confirm_like("reset_password", username, err, "");
    Response r; r.status=400; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }
  RegisteredUser u;
  if(!find_registered_user(username, u) || u.status!="active" || u.otp_code.empty()){
    // User tak ditemukan / tidak punya OTP aktif → pesan seragam.
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Kode OTP salah atau sudah tidak berlaku.\"}"); return r; }
    RenderedAuthPage rp=page_register_confirm_like("reset_password", username, "Kode OTP salah atau sudah tidak berlaku.", "");
    Response r; r.status=400; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }
  long now=now_epoch();
  if(u.otp_expiry_epoch>0 && now>u.otp_expiry_epoch){
    update_user_otp(username, "", 0); // nonaktifkan OTP kedaluwarsa
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Kode OTP sudah kedaluwarsa.\"}"); return r; }
    RenderedAuthPage rp=page_register_confirm_like("reset_password", username, "Kode OTP sudah kedaluwarsa.", "");
    Response r; r.status=400; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }
  if(otp!=u.otp_code){
    bump_otp_attempts(username);
    if(u.otp_attempts+1>=kMaxOtpAttempts){
      update_user_otp(username, "", 0); // nonaktifkan OTP setelah 5x salah
      if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Terlalu banyak percobaan. Minta kode baru.\"}"); return r; }
      RenderedAuthPage rp=page_register_confirm_like("reset_password", username, "Terlalu banyak percobaan. Minta kode baru.", "");
      Response r; r.status=400; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
      r.body=rp.body; return r;
    }
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Kode OTP salah atau sudah tidak berlaku.\"}"); return r; }
    RenderedAuthPage rp=page_register_confirm_like("reset_password", username, "Kode OTP salah atau sudah tidak berlaku.", "");
    Response r; r.status=400; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }
  // OTP benar → update password + bersihkan OTP.
  update_user_password(username, helpers::hash_password(password));
  if(wants_json(req)){ Response r; r.json(200, "{\"success\":true,\"message\":\"Password berhasil diubah. Silakan login.\",\"next\":\"/login\"}"); return r; }
  Response r; r.status=303; r.headers["Location"]="/login"; return r;
}

} // namespace examvan::handlers::auth