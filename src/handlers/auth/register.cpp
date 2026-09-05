#include "handlers/auth/register.hpp"
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

// Jumlah tebakan OTP sebelum akun dihapus / OTP dinonaktifkan.
constexpr int kMaxOtpAttempts = 5;
// Masa berlaku OTP (detik).
constexpr long kOtpTtlSec = 15 * 60;
// Cooldown resend (detik).
constexpr long kResendCooldownSec = 60;
// Regex username public: huruf kecil, angka, titik, garis bawah, min 3.
bool valid_username(const std::string& u){
  if(u.size()<3 || u.size()>32) return false;
  for(char c: u){
    if(!(c>='a'&&c<='z') && !(c>='0'&&c<='9') && c!='.' && c!='_') return false;
  }
  return true;
}
bool valid_email(const std::string& e){
  size_t at=e.find('@');
  if(at==std::string::npos || at==0 || at==e.size()-1) return false;
  size_t dot=e.find('.', at+1);
  return dot!=std::string::npos && dot<e.size()-1;
}
bool valid_password(const std::string& p){ return p.size()>=8; }

// Username cadangan admin dilarang untuk registrasi public.
bool reserved_username(const std::string& u){
  return u=="admin" || u=="superadmin" || u=="administrator" || u=="root";
}

std::string gen_otp(){
  // 6 digit dari CSPRNG; fallback ke mt19937 bila RAND_bytes gagal.
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

// Kunci rate limit: nama-alur + IP.
static std::string rl_key(const char* flow, const std::string& ip){ return std::string(flow)+":"+ip; }

std::string get_setting_str(const char* k, const std::string& def){ return get_setting(k, def); }

} // namespace

// Per-IP register 5/jam + max_accounts_per_ip dari saas_settings.
// (Di luar anon-namespace agar reset_register_limit_for_test punya linkage
// eksternal untuk dipanggil suite test.)
static middleware::RateLimiter g_reg_ip(5, std::chrono::hours(1));
void reset_register_limit_for_test(){ g_reg_ip.reset(); }
std::string check_register_limit(const std::string& ip){
  if(!g_reg_ip.allow("register:"+ip)) return "Terlalu banyak pendaftaran dari IP ini. Coba lagi nanti.";
  std::string m=get_setting("max_accounts_per_ip","3");
  int max_per_ip=3;
  try{ max_per_ip=std::stoi(m); }catch(...){}
  if(max_per_ip>0){
    int n=count_recent_registrations_by_ip(ip);
    if(n>=max_per_ip) return "Terlalu banyak akun dibuat dari IP ini. Coba lagi besok.";
  }
  return "";
}

// ============================= GET /register =============================
Response register_page(const Request& req){
  // Sudah login? → dashboard admin.
  auto cfg=Config::load();
  std::string cookie_hdr=get_hdr_ci(req,"Cookie");
  if(!cookie_hdr.empty()){
    std::string s=cfg.secret_key, prev=cfg.secret_prev;
    auto sess=prev.empty()? verify_session_cookie(s, cookie_hdr) : verify_session_cookie_dual(s, prev, cookie_hdr);
    if(sess){
      Response r; r.status=302; r.headers["Location"]="/admin/dashboard"; return r;
    }
  }
  PublicAuthPage p;
  p.name="register";
  p.email_enabled=get_setting_str("email_verification_enabled","0")=="1";
  p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
  p.turnstile_site_key=get_setting_str("turnstile_site_key","");
  RenderedAuthPage rp=render_auth_page(p);
  Response res; res.status=200; res.headers["Content-Type"]="text/html";
  res.headers["Set-Cookie"]=rp.set_cookie;
  res.body=rp.body.empty()? "<html>Register</html>" : rp.body;
  return res;
}

// ============================= POST /register =============================
Response register_handler(const Request& req, const Config& cfg){
  auto form=helpers::parse_form(req.body);
  std::string ip=client_ip(req);
  std::string e_limit=check_register_limit(ip);
  if(!e_limit.empty()){
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\""+e_limit+"\"}"); return r; }
    PublicAuthPage p; p.name="register"; p.error=e_limit;
    p.form_username=form.count("username")? form["username"]:"";
    p.form_email=form.count("email")? form["email"]:"";
    p.email_enabled=get_setting_str("email_verification_enabled","0")=="1";
    p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
    p.turnstile_site_key=get_setting_str("turnstile_site_key","");
    RenderedAuthPage rp=render_auth_page(p);
    Response r; r.status=429; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }
  if(!csrf_ok(req, form)){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}");
    return r;
  }
  std::string username=form.count("username")? form["username"]:"";
  std::string email=form.count("email")? form["email"]:"";
  std::string password=form.count("password")? form["password"]:"";
  std::string password_confirm=form.count("password_confirm")? form["password_confirm"]:"";
  std::string turnstile=form.count("cf-turnstile-response")? form["cf-turnstile-response"]:"";
  // Normalisasi username (lowercase) — template & DB memakai lower.
  for(char &c: username) c=tolower((unsigned char)c);
  username=helpers::sanitize_student_input(username);
  email=helpers::sanitize_student_input(email);

  std::string err;
  if(username.empty() || email.empty() || password.empty()) err="Semua field wajib diisi.";
  else if(!valid_username(username)) err="Username 3-32 karakter (huruf kecil, angka, titik, garis bawah).";
  else if(!valid_email(email)) err="Format email tidak valid.";
  else if(!valid_password(password)) err="Password minimal 8 karakter.";
  else if(password.size()>72) err="Password maksimal 72 karakter (batas bcrypt).";
  else if(password!=password_confirm) err="Konfirmasi password tidak cocok.";
  else if(reserved_username(username)) err="Username tersebut tidak tersedia.";
  if(err.empty()){
    bool ts_enabled=get_setting_str("turnstile_enabled","0")=="1";
    if(ts_enabled){
      std::string secret=cfg.turnstile_secret.empty()? get_setting_str("turnstile_secret_key","") : cfg.turnstile_secret;
      if(!middleware::verify_turnstile(turnstile, secret, ip)) err="Verifikasi keamanan gagal. Coba lagi.";
    }
  }
  if(err.empty()){
    // Whitelist domain email (bila dikonfigurasi).
    std::string wl=get_setting_str("email_domain_whitelist","");
    if(!wl.empty()){
      size_t at=email.find('@');
      std::string dom=at==std::string::npos? "" : email.substr(at+1);
      bool ok=false;
      size_t p=0;
      while(p<=wl.size()){
        size_t c=wl.find(',',p);
        std::string d=wl.substr(p, c==std::string::npos? std::string::npos : c-p);
        size_t a=d.find_first_not_of(" \t"); size_t b=d.find_last_not_of(" \t");
        if(a!=std::string::npos) d=d.substr(a,b-a+1);
        if(!d.empty() && d==dom){ ok=true; break; }
        if(c==std::string::npos) break;
        p=c+1;
      }
      if(!ok) err="Domain email tidak diizinkan untuk pendaftaran.";
    }
  }
  if(err.empty()){
    // Unik: username / email sudah dipakai?
    RegisteredUser existing;
    if(find_registered_user(username, existing)) err="Username sudah terdaftar.";
    else if(find_registered_user_by_email(email, existing)) err="Email sudah terdaftar.";
  }
  if(!err.empty()){
    // Re-render form + error, echo username/email (password tidak pernah).
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\""+err+"\"}"); return r; }
    PublicAuthPage p; p.name="register"; p.error=err;
    p.form_username=username; p.form_email=email;
    p.email_enabled=get_setting_str("email_verification_enabled","0")=="1";
    p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
    p.turnstile_site_key=get_setting_str("turnstile_site_key","");
    RenderedAuthPage rp=render_auth_page(p);
    Response r; r.status=400; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }

  // OK: INSERT user (status=active bila verifikasi email mati, pending_otp bila hidup).
  bool email_verify=get_setting_str("email_verification_enabled","0")=="1";
  RegisteredUser u;
  u.username=username;
  u.email=email;
  u.password_hash=helpers::hash_password(password);
  u.status=email_verify? "pending_otp" : "active";
  u.registered_ip=ip;
  try{ u.max_exams=std::stoi(get_setting_str("default_max_exams","3")); }catch(...){ u.max_exams=3; }
  try{ u.max_pdf_size=std::stol(get_setting_str("default_max_pdf_size","1048576")); }catch(...){ u.max_pdf_size=1048576; }
  try{ u.max_concurrent_exams=std::stoi(get_setting_str("default_max_concurrent_exams","2")); }catch(...){ u.max_concurrent_exams=2; }
  try{ u.max_storage_size=std::stol(get_setting_str("default_max_storage_size","52428800")); }catch(...){ u.max_storage_size=52428800; }
  try{ u.active_days=std::stoi(get_setting_str("default_active_days","14")); }catch(...){ u.active_days=14; }
  if(email_verify){
    u.otp_code=gen_otp();
    u.otp_expiry_epoch=now_epoch()+kOtpTtlSec;
  }
  if(!insert_registered_user(u)){
    // Race / duplicate — konflik tidak membocorkan akun mana yang bentrok.
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Username atau email sudah terdaftar.\"}"); return r; }
    PublicAuthPage p; p.name="register"; p.error="Username atau email sudah terdaftar.";
    p.form_username=username; p.form_email=email;
    p.email_enabled=get_setting_str("email_verification_enabled","0")=="1";
    p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
    p.turnstile_site_key=get_setting_str("turnstile_site_key","");
    RenderedAuthPage rp=render_auth_page(p);
    Response r; r.status=400; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }

  if(email_verify){
    // Kirim email OTP. Gagal kirim → batalkan pendaftaran (fail-closed).
    std::string host=get_setting_str("smtp_host","smtp.gmail.com");
    std::string port=get_setting_str("smtp_port","587");
    std::string user=get_setting_str("smtp_user","");
    std::string pass=get_setting_str("smtp_password","");
    std::string sender=get_setting_str("smtp_sender_name","EXAMVAN");
    std::string mail_err=helpers::send_verification_email(host, port, user, pass, sender, u.email, u.username, u.otp_code);
    if(!mail_err.empty()){
      delete_registered_user(u.username);
      if(wants_json(req)){ Response r; r.json(500, "{\"error\":\"Gagal mengirim email verifikasi. Coba lagi nanti.\"}"); return r; }
      PublicAuthPage p; p.name="register"; p.error="Gagal mengirim email verifikasi. Coba lagi nanti.";
      p.form_username=username; p.form_email=email;
      p.email_enabled=true; p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
      p.turnstile_site_key=get_setting_str("turnstile_site_key","");
      RenderedAuthPage rp=render_auth_page(p);
      Response r; r.status=500; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
      r.body=rp.body; return r;
    }
    return success_response(wants_json(req), "/register/confirm?username="+url_encode(u.username),
                            "{\"success\":true,\"next\":\"/register/confirm?username="+url_encode(u.username)+"\"}");
  }
  // Verifikasi email mati → langsung aktif, redirect ke login.
  return success_response(wants_json(req), "/login",
                          "{\"success\":true,\"message\":\"Akun berhasil dibuat. Silakan login.\",\"next\":\"/login\"}");
}

// ========================= GET /register/confirm =========================
Response register_confirm_page(const Request& req){
  auto q=helpers::parse_form(req.query);
  std::string username=q.count("username")? q["username"]:"";
  PublicAuthPage p; p.name="register_confirm";
  p.email_enabled=true;
  p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1";
  p.turnstile_site_key=get_setting_str("turnstile_site_key","");
  // Netral: halaman SELALU dirender (200) baik user pending maupun tidak —
  // menghindari oracle enumerasi (200 vs 302 membocorkan status pending).
  // Email termasking hanya diisi bila benar-benar user pending.
  if(!username.empty()){
    RegisteredUser u;
    if(find_registered_user(username, u) && u.status=="pending_otp" && !u.otp_code.empty()){
      p.username=u.username;
      // Mask email: a***@domain.
      size_t at=u.email.find('@');
      std::string masked=u.email;
      if(at!=std::string::npos){
        std::string local=u.email.substr(0,at);
        std::string dom=u.email.substr(at);
        masked = local.empty()? "***" : (local.substr(0,1)+std::string("***")+ (local.size()>1? local.substr(local.size()-1):""));
        masked+=dom;
      }
      p.masked_email=masked;
    } else {
      // User tidak ditemukan / bukan pending → halaman tetap dirender netral
      // (tanpa email); POST akan ditolak seragam seperti sebelumnya.
      p.username=username;
    }
  }
  RenderedAuthPage rp=render_auth_page(p);
  Response r; r.status=200; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
  r.body=rp.body; return r;
}

// ========================= POST /register/confirm =========================
Response register_confirm_handler(const Request& req, const Config& cfg){
  auto q=helpers::parse_form(req.query);
  std::string username=q.count("username")? q["username"]:"";
  auto form=helpers::parse_form(req.body);
  if(username.empty() || !csrf_ok(req, form)){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}");
    return r;
  }
  std::string otp=form.count("otp_code")? form["otp_code"]:"";
  std::string ip=client_ip(req);
  static middleware::RateLimiter g_confirm_rl(5, std::chrono::minutes(1));
  if(!g_confirm_rl.allow(rl_key("confirm", ip))){
    if(wants_json(req)){ Response r; r.json(429, "{\"error\":\"Terlalu banyak percobaan. Coba lagi nanti.\"}"); return r; }
    PublicAuthPage p; p.name="register_confirm"; p.username=username; p.error="Terlalu banyak percobaan. Coba lagi nanti.";
    p.email_enabled=true; p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1"; p.turnstile_site_key=get_setting_str("turnstile_site_key","");
    RenderedAuthPage rp=render_auth_page(p);
    Response r; r.status=429; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }
  RegisteredUser u;
  if(!find_registered_user(username, u) || u.status!="pending_otp" || u.otp_code.empty()){
    // User tidak ada / bukan pending → netral, tidak membocorkan status.
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Kode OTP salah atau sudah tidak berlaku.\"}"); return r; }
    Response r; r.status=302; r.headers["Location"]="/login"; return r;
  }
  long now=now_epoch();
  if(u.otp_expiry_epoch>0 && now>u.otp_expiry_epoch){
    // Kedaluwarsa → hapus user (anti penimbunan akun pending).
    delete_registered_user(username);
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Kode OTP kedaluwarsa. Silakan daftar ulang.\"}"); return r; }
    Response r; r.status=302; r.headers["Location"]="/login"; return r;
  }
  if(otp!=u.otp_code){
    bump_otp_attempts(username);
    if(u.otp_attempts+1>=kMaxOtpAttempts){
      // Nonaktifkan OTP (bukan hapus akun) — mencegah CSRF-DoS: penyerang
      // yang memaksa browser korban POST 5× OTP salah tidak boleh menghapus
      // akun; kode OTP dinonaktifkan sehingga percobaan berikutnya ditolak.
      update_user_otp(username, "", 0);
      if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Terlalu banyak percobaan. Minta kode baru.\"}"); return r; }
      Response r; r.status=302; r.headers["Location"]="/login"; return r;
    }
    if(wants_json(req)){ Response r; r.json(400, "{\"error\":\"Kode OTP salah atau sudah tidak berlaku.\"}"); return r; }
    PublicAuthPage p; p.name="register_confirm"; p.username=username; p.error="Kode OTP salah.";
    p.email_enabled=true; p.turnstile_enabled=get_setting_str("turnstile_enabled","0")=="1"; p.turnstile_site_key=get_setting_str("turnstile_site_key","");
    RenderedAuthPage rp=render_auth_page(p);
    Response r; r.status=400; r.headers["Content-Type"]="text/html"; r.headers["Set-Cookie"]=rp.set_cookie;
    r.body=rp.body; return r;
  }
  // Benar → aktifkan, bersihkan OTP.
  activate_registered_user(username);
  if(wants_json(req)){ Response r; r.json(200, "{\"success\":true,\"next\":\"/login\"}"); return r; }
  Response r; r.status=303; r.headers["Location"]="/login"; return r;
}

// ========================= POST /register/resend =========================
Response resend_otp(const Request& req, const Config& cfg){
  auto q=helpers::parse_form(req.query);
  std::string username=q.count("username")? q["username"]:"";
  auto form=helpers::parse_form(req.body);
  if(username.empty() || !csrf_ok(req, form)){
    Response r; r.status=403; r.json(403,"{\"error\":\"CSRF token mismatch\"}"); return r;
  }
  std::string ip=client_ip(req);
  static middleware::RateLimiter g_resend_rl(5, std::chrono::minutes(1));
  if(!g_resend_rl.allow(rl_key("resend", ip))){
    Response r; r.status=429; r.json(429,"{\"error\":\"Terlalu banyak permintaan. Coba lagi nanti.\"}"); return r;
  }
  RegisteredUser u;
  long now=now_epoch();
  bool cooldown_ok=false;
  if(find_registered_user(username, u) && u.status=="pending_otp"){
    // Cooldown 60 detik. otp_expiry_epoch==0 → tidak ada catatan → izinkan.
    long last = u.otp_expiry_epoch - kOtpTtlSec; // saat OTP terakhir dibuat
    if(u.otp_code.empty() || u.otp_expiry_epoch<=0 || now >= last + kResendCooldownSec) cooldown_ok=true;
    if(cooldown_ok){
      std::string new_otp=gen_otp();
      update_user_otp(username, new_otp, now+kOtpTtlSec);
      std::string host=get_setting_str("smtp_host","smtp.gmail.com");
      std::string port=get_setting_str("smtp_port","587");
      std::string user=get_setting_str("smtp_user","");
      std::string pass=get_setting_str("smtp_password","");
      std::string sender=get_setting_str("smtp_sender_name","EXAMVAN");
      helpers::send_verification_email(host, port, user, pass, sender, u.email, u.username, new_otp);
    }
  }
  // Respons seragam untuk semua cabang (anti enumerasi akun).
  (void)cfg;
  Response r; r.json(200,"{\"success\":true,\"message\":\"Kode verifikasi telah dikirim.\"}");
  return r;
}

} // namespace examvan::handlers::auth