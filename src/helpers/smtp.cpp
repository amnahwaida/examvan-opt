#include "helpers/smtp.hpp"
#include "utils/sanitize.hpp"
#ifdef HAS_LIBCURL
#include <curl/curl.h>
#endif
#include <sstream>
#include <cctype>

namespace examvan::helpers {

// Paritas Go helpers/email.go: pesan MIME ber-format TIDAK boleh berisi
// baris kosong di tengah (header harus langsung diikuti body), dan CRLF wajib
// untuk protokol SMTP.
static std::string smtp_message(const std::string& from_header,
                                const std::string& to_header,
                                const std::string& subject,
                                const std::string& html_body){
  std::ostringstream os;
  os << "From: " << from_header << "\r\n"
     << "To: " << to_header << "\r\n"
     << "Subject: " << subject << "\r\n"
     << "MIME-Version: 1.0\r\n"
     << "Content-Type: text/html; charset=UTF-8\r\n"
     << "Content-Transfer-Encoding: base64\r\n\r\n";
  // Body di-base64 agar aman untuk transfer (termasuk karakter UTF-8 nama
  // user / pesan) — sama seperti Go mime/quoted-printable tujuan yang sama.
  const std::string raw=html_body;
  std::string b64;
  b64.reserve((raw.size()+2)/3*4);
  static const char* tbl="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i=0;
  for(; i+3<=raw.size(); i+=3){
    unsigned v=(unsigned char)raw[i]<<16 | (unsigned char)raw[i+1]<<8 | (unsigned char)raw[i+2];
    b64.push_back(tbl[(v>>18)&63]); b64.push_back(tbl[(v>>12)&63]);
    b64.push_back(tbl[(v>>6)&63]);  b64.push_back(tbl[v&63]);
  }
  if(i+2==raw.size()){
    unsigned v=(unsigned char)raw[i]<<16 | (unsigned char)raw[i+1]<<8;
    b64.push_back(tbl[(v>>18)&63]); b64.push_back(tbl[(v>>12)&63]);
    b64.push_back(tbl[(v>>6)&63]);  b64.push_back('=');
  } else if(i+1==raw.size()){
    unsigned v=(unsigned char)raw[i]<<16;
    b64.push_back(tbl[(v>>18)&63]); b64.push_back(tbl[(v>>12)&63]);
    b64.push_back('='); b64.push_back('=');
  }
  // Line-wrap base64 @ 76 kolom (wajib SMTP; Go melakukan hal yang sama via
  // mime/base64 encoding dalam satu baris? tidak — Go memakai email writer
  // yang men-wrap. Kita wrap di sini agar server ketat tidak menolak).
  for(size_t p=0; p<b64.size(); p+=76){
    os.write(b64.data()+p, std::min<size_t>(76, b64.size()-p));
    os << "\r\n";
  }
  os << ".\r\n";
  return os.str();
}

static std::string plain_email(const std::string& s){
  std::string o; o.reserve(s.size());
  for(char c: s) if(c!='\r' && c!='\n') o.push_back(c);
  return o;
}

#ifdef HAS_LIBCURL
namespace {
size_t smtp_discard_cb(char*, size_t size, size_t nmemb, void*){ return size*nmemb; }
}

static std::string send_via_curl(const std::string& host, const std::string& port,
                                 const std::string& user, const std::string& password,
                                 const std::string& from_addr, const std::string& to,
                                 const std::string& message){
  CURL* curl=curl_easy_init();
  if(!curl) return "curl init failed";
  std::string url="smtp://"+host+":"+port;
  struct curl_slist* rcpt=nullptr;
  rcpt=curl_slist_append(rcpt, to.c_str());
  std::string errbuf(CURL_ERROR_SIZE, '\0');
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_MAIL_FROM, from_addr.c_str());
  curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, rcpt);
  curl_easy_setopt(curl, CURLOPT_READFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata)->size_t{
    auto* st=static_cast<std::istringstream*>(userdata);
    st->read(ptr, static_cast<std::streamsize>(size*nmemb));
    return static_cast<size_t>(st->gcount());
  });
  std::istringstream payload(message);
  curl_easy_setopt(curl, CURLOPT_READDATA, &payload);
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_USERNAME, user.empty()? nullptr:user.c_str());
  curl_easy_setopt(curl, CURLOPT_PASSWORD, password.empty()? nullptr:password.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, smtp_discard_cb);
  if(port=="465") curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
  else            curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_TRY);
  if(!user.empty()) curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL); // creds → jalur aman
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf.data());
  CURLcode rc=curl_easy_perform(curl);
  std::string err;
  if(rc!=CURLE_OK) err="smtp send failed: "+std::string(errbuf.c_str()?errbuf.c_str():"")+" ("+curl_easy_strerror(rc)+")";
  curl_slist_free_all(rcpt);
  curl_easy_cleanup(curl);
  return err;
}
#endif

std::string send_smtp_message(const std::string& host, const std::string& port,
                              const std::string& user, const std::string& password,
                              const std::string& sender_name, const std::string& to,
                              const std::string& subject, const std::string& html_body){
  if(host.empty() || user.empty() || to.empty()) return "smtp not configured";
  std::string from_addr=plain_email(user);
  std::string from_header=plain_email(sender_name)+" <"+from_addr+">";
  std::string to_header="<"+plain_email(to)+">";
  std::string msg=smtp_message(from_header, to_header, plain_email(subject), html_body);
#ifdef HAS_LIBCURL
  return send_via_curl(host, port, user, password, from_addr, to, msg);
#else
  (void)port; (void)password; (void)msg;
  return "SMTP unavailable (no libcurl)";
#endif
}

std::string test_smtp_connection(const std::string& host, const std::string& port,
                                 const std::string& user, const std::string& password){
  if(host.empty() || port.empty()) return "SMTP Host dan SMTP Port tidak boleh kosong";
#ifdef HAS_LIBCURL
  CURL* curl=curl_easy_init(); if(!curl) return "curl init failed";
  std::string url="smtp://"+host+":"+port;
  std::string errbuf(CURL_ERROR_SIZE,'\0');
  curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
  curl_easy_setopt(curl,CURLOPT_CONNECT_ONLY,1L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,8L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,10L);
  curl_easy_setopt(curl,CURLOPT_USERNAME,user.empty()?nullptr:user.c_str());
  curl_easy_setopt(curl,CURLOPT_PASSWORD,password.empty()?nullptr:password.c_str());
  curl_easy_setopt(curl,CURLOPT_ERRORBUFFER,errbuf.data());
  if(port=="465") curl_easy_setopt(curl,CURLOPT_USE_SSL,CURLUSESSL_ALL);
  else curl_easy_setopt(curl,CURLOPT_USE_SSL,CURLUSESSL_TRY);
  CURLcode rc=curl_easy_perform(curl);
  std::string err;
  if(rc!=CURLE_OK) err="smtp connection failed: "+std::string(errbuf.c_str())+" ("+curl_easy_strerror(rc)+")";
  curl_easy_cleanup(curl); return err;
#else
  (void)user; (void)password; return "SMTP unavailable (no libcurl)";
#endif
}

std::string send_verification_email(const std::string& host, const std::string& port,
                                    const std::string& user, const std::string& password,
                                    const std::string& sender_name, const std::string& to,
                                    const std::string& username, const std::string& otp){
  std::string body=
    "<div style=\"font-family:Arial,sans-serif;max-width:480px;margin:0 auto;padding:24px;border:1px solid #e5e7eb;border-radius:12px;\">"
    "<h2 style=\"margin:0 0 8px;color:#1f2937;\">Verifikasi Pendaftaran Akun EXAMVAN</h2>"
    "<p style=\"color:#4b5563;font-size:14px;line-height:1.6;\">Halo <b>"+html_escape(username)+"</b>, terima kasih telah mendaftar.</p>"
    "<p style=\"color:#4b5563;font-size:14px;line-height:1.6;\">Gunakan kode OTP berikut untuk memverifikasi akun Anda:</p>"
    "<p style=\"font-size:28px;font-weight:700;letter-spacing:8px;color:#2563eb;text-align:center;margin:20px 0;\">"+html_escape(otp)+"</p>"
    "<p style=\"color:#9ca3af;font-size:12px;\">Kode berlaku 15 menit. Abaikan email ini bila Anda tidak mendaftar.</p>"
    "</div>";
  return send_smtp_message(host, port, user, password, sender_name, to, "Verifikasi Pendaftaran Akun EXAMVAN", body);
}

std::string send_password_reset_email(const std::string& host, const std::string& port,
                                      const std::string& user, const std::string& password,
                                      const std::string& sender_name, const std::string& to,
                                      const std::string& username, const std::string& otp){
  std::string body=
    "<div style=\"font-family:Arial,sans-serif;max-width:480px;margin:0 auto;padding:24px;border:1px solid #e5e7eb;border-radius:12px;\">"
    "<h2 style=\"margin:0 0 8px;color:#1f2937;\">Reset Password Akun EXAMVAN</h2>"
    "<p style=\"color:#4b5563;font-size:14px;line-height:1.6;\">Halo <b>"+html_escape(username)+"</b>, kami menerima permintaan reset password.</p>"
    "<p style=\"color:#4b5563;font-size:14px;line-height:1.6;\">Gunakan kode OTP berikut untuk membuat password baru:</p>"
    "<p style=\"font-size:28px;font-weight:700;letter-spacing:8px;color:#2563eb;text-align:center;margin:20px 0;\">"+html_escape(otp)+"</p>"
    "<p style=\"color:#9ca3af;font-size:12px;\">Kode berlaku 15 menit. Abaikan email ini bila Anda tidak meminta reset password.</p>"
    "</div>";
  return send_smtp_message(host, port, user, password, sender_name, to, "Reset Password Akun EXAMVAN", body);
}

} // namespace examvan::helpers