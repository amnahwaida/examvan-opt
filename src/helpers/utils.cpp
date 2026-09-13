#include "helpers/utils.hpp"
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <openssl/rand.h>

namespace examvan::helpers {

std::string format_iso_utc(std::chrono::system_clock::time_point tp){
  std::time_t t = std::chrono::system_clock::to_time_t(tp);
  char buf[32];
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm,&t);
#else
  gmtime_r(&t, &tm);
#endif
  std::strftime(buf,sizeof(buf),"%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::optional<std::chrono::system_clock::time_point> parse_iso_utc(const std::string& s){
  std::tm tm{}; std::istringstream ss(s);
  ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  if(ss.fail()) return std::nullopt;
  return std::chrono::system_clock::from_time_t(timegm(&tm));
}

/* P33-Ef: parser waktu KETAT fail-closed — menerima format `timestamptz` PG
 * ("YYYY-MM-DD HH:MM:SS[.fff][+HH|+HH:MM|+HHMM]") maupun ISO-8601
 * ("YYYY-MM-DDTHH:MM[:SS]Z"). Tanpa offset zona → dianggap UTC (paritas
 * tampilan PG default DateStyle ISO). Prinsip: SATU karakter pun menyimpang
 * dari format yang dikenal → nullopt (pemanggil wajib memperlakukan voucher
 * TIDAK usable, bukan melewati cek kedaluwarsa).
 * Berbeda dari parse_iso_utc (get_time longgar, format tunggal, tanpa
 * validasi rentang): di sini field divalidasi ketat termasuk hari-dalam-bulan
 * ("2026-02-31" ditolak). */
std::optional<std::chrono::system_clock::time_point> parse_pg_or_iso_utc(const std::string& s){
  if(s.size()<19) return std::nullopt;
  auto dig=[&](size_t i)->int{ char c=s[i]; if(c<'0'||c>'9') return -1; return c-'0'; };
  auto two=[&](size_t i)->int{ int a=dig(i), b=dig(i+1); if(a<0||b<0) return -1; return a*10+b; };
  // Tanggal: YYYY-MM-DD (indeks 0..9)
  int y=dig(0)*1000+dig(1)*100+dig(2)*10+dig(3);
  if(dig(0)<0||dig(1)<0||dig(2)<0||dig(3)<0||s[4]!='-'||s[7]!='-') return std::nullopt;
  int mo=two(5), d=two(8);
  if(mo<1||mo>12||d<1||d>31) return std::nullopt;
  // Separator: 'T' (ISO) atau spasi (PG)
  if(s[10]!='T' && s[10]!=' ') return std::nullopt;
  // Waktu: HH:MM:SS (indeks 11..18)
  int h=two(11), mi=two(14), sec=two(17);
  if(h<0||h>23||mi<0||mi>59||sec<0||sec>60) return std::nullopt;
  if(s[13]!=':'||s[16]!=':') return std::nullopt;
  int off_min=0, frac_ms=0;
  size_t pos=19;
  // Fraksi detik opsional: .f[.f...] — maksimal 3 digit dipakai (presisi ms).
  if(pos<s.size() && s[pos]=='.'){
    ++pos; int nd=0; long frac=0;
    while(pos<s.size() && dig(pos)>=0){ if(nd<3) frac=frac*10+dig(pos); ++nd; ++pos; }
    if(nd==0) return std::nullopt;
    while(nd<3){ frac*=10; ++nd; }
    frac_ms=(int)frac;
  }
  // Zona opsional: Z | +HH | +HHMM | +HH:MM (minus untuk offset barat).
  if(pos<s.size()){
    char c=s[pos];
    if(c=='Z'){
      ++pos;
    } else if(c=='+'||c=='-'){
      int sign=(c=='-')? -1 : 1;
      size_t rem=s.size()-(pos+1);
      int oh=-1, om=0;
      if(rem==2){ oh=two(pos+1); }
      else if(rem==4){ int a=dig(pos+1),b=dig(pos+2),c2=dig(pos+3),d2=dig(pos+4);
        if(a>=0&&b>=0&&c2>=0&&d2>=0){ oh=a*10+b; om=c2*10+d2; } }
      else if(rem==5 && s[pos+3]==':'){ oh=two(pos+1); om=two(pos+4); }
      else return std::nullopt;
      if(oh<0||oh>23||om<0||om>59) return std::nullopt;
      off_min=sign*(oh*60+om);
      pos=s.size();
    } else {
      return std::nullopt;
    }
  }
  if(pos!=s.size()) return std::nullopt;
  // Validasi hari-dalam-bulan ("2026-02-31" → nullopt; "2024-02-29" kabisat OK).
  static const int mdays[]={31,28,31,30,31,30,31,31,30,31,30,31};
  int maxd=mdays[mo-1];
  if(mo==2 && ((y%4==0&&y%100!=0)||y%400==0)) maxd=29;
  if(d>maxd) return std::nullopt;
  std::tm tm{};
  tm.tm_year=y-1900; tm.tm_mon=mo-1; tm.tm_mday=d;
  tm.tm_hour=h; tm.tm_min=mi; tm.tm_sec=sec;
  auto tp=std::chrono::system_clock::from_time_t(timegm(&tm));
  if(off_min!=0) tp-=std::chrono::minutes(off_min);
  if(frac_ms!=0) tp+=std::chrono::milliseconds(frac_ms);
  return tp;
}

std::string sanitize_student_input(const std::string& s){
  std::string out; out.reserve(s.size());
  bool last_space=false;
  for(char c: s){
    if(std::isspace((unsigned char)c)){ if(!last_space) out.push_back(' '); last_space=true; }
    else { out.push_back(c); last_space=false; }
  }
  size_t a=out.find_first_not_of(' '); if(a==std::string::npos) return "";
  size_t b=out.find_last_not_of(' '); return out.substr(a,b-a+1);
}

std::string generate_token(int len){
  static const char* chars="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  unsigned char buf[64];
  if(RAND_bytes(buf, sizeof(buf))==1){
    std::string s; s.reserve(len);
    for(int i=0;i<len;i++) s.push_back(chars[buf[i] % 32]);
    return s;
  }
  // fallback (jika RAND_bytes gagal, sangat jarang)
  std::random_device rd; std::mt19937 g(rd());
  std::uniform_int_distribution<> d(0,31);
  std::string s; s.reserve(len); for(int i=0;i<len;i++) s.push_back(chars[d(g)]); return s;
}

std::string localize_utc(const std::string& utc_str, int offset_minutes){
  auto tp=parse_iso_utc(utc_str); if(!tp) return utc_str;
  auto local = *tp + std::chrono::minutes(offset_minutes);
  return format_iso_utc(local);
}

bool is_valid_exam_token(const std::string& t){
  if(t.size()<6||t.size()>32) return false;
  return std::all_of(t.begin(), t.end(), [](char c){ return std::isalnum((unsigned char)c); });
}

std::string round_to(double v, int decimals){
  std::ostringstream ss; ss<< std::fixed<< std::setprecision(decimals)<<v; return ss.str();
}

/* P33-F4: whitelist versi — charset [A-Za-z0-9.-], panjang 1..64.
 * Nilai X-Version dari klien hanya dipakai untuk rendering bila lolos cek ini
 * (mencegah attribute-breakout di href asset: `2.7.3" onclick=...`). */
bool is_safe_version(const std::string& s){
  if(s.empty() || s.size()>64) return false;
  for(char c: s){
    bool ok=(c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='-';
    if(!ok) return false;
  }
  return true;
}

std::string url_decode(const std::string& s){
  std::string out; out.reserve(s.size());
  auto hex=[](char c)->int{
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
  };
  for(size_t i=0;i<s.size();++i){
    if(s[i]=='+'){ out+=' '; }
    else if(s[i]=='%' && i+2<s.size()){
      int h=hex(s[i+1]), l=hex(s[i+2]);
      if(h>=0&&l>=0){ out+=static_cast<char>((h<<4)|l); i+=2; }
      else out+=s[i];
    } else out+=s[i];
  }
  return out;
}

std::map<std::string,std::string> parse_form(const std::string& body){
  std::map<std::string,std::string> m;
  size_t start=0;
  while(start<=body.size()){
    size_t amp=body.find('&',start);
    std::string pair=body.substr(start, amp==std::string::npos? std::string::npos : amp-start);
    if(!pair.empty()){
      size_t eq=pair.find('=');
      std::string k = eq==std::string::npos? pair : pair.substr(0,eq);
      std::string v = eq==std::string::npos? "" : pair.substr(eq+1);
      m[url_decode(k)]=url_decode(v);
    }
    if(amp==std::string::npos) break;
    start=amp+1;
  }
  return m;
}

// ---- Konversi jadwal WIB → UTC ISO (paritas Go SaveQuestions) ----
// Go menerima "YYYY-MM-DD HH:MM" (Asia/Jakarta, UTC+7 tanpa DST) lalu
// menyimpan UTC ISO "YYYY-MM-DDTHH:MM:SSZ". Dipindah dari exams.cpp (P36-D6)
// agar terkunci test unit langsung.
namespace {
int days_from_civil(int y, unsigned m, unsigned d){
  y -= (int)(m <= 2);
  const int era = (y >= 0 ? y : y-399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u*(m + (m > 2 ? -3 : 9)) + 2u)/5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe/4u - yoe/100u + doy;
  return era * 146097 + (int)doe - 719468;
}
void civil_from_days(int z, int& y, unsigned& m, unsigned& d){
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe/1460u + doe/36524u - doe/146096u) / 365u;
  const int y2 = (int)yoe + era * 400;
  const unsigned doy = doe - (365u*yoe + yoe/4u - yoe/100u);
  const unsigned mp = (5u*doy + 2u)/153u;
  const unsigned d2 = doy - (153u*mp + 2u)/5u + 1u;
  const unsigned m2 = mp < 10u ? mp + 3u : mp - 9u;
  y = y2 + (int)(m2 <= 2u); m = m2; d = d2;
}
} // namespace

std::optional<std::string> wib_to_utc_iso(const std::string& s){
  /* P36-D6: parser KETAT — format tunggal "YYYY-MM-DD HH:MM[:SS]" dan WAJIB
   * dikonsumsi utuh (dulu sscanf 5 field tanpa %n: "2026-05-01 10:00JUNK"
   * diterima). Rentang divalidasi termasuk hari-dalam-bulan (dulu hanya
   * d<=31: "2026-02-31" menjadi 3 Maret). Tanpa ini, jadwal soal rusak
   * tersimpan dan scoring/Android gagal saat ujian berlangsung. */
  if(s.size()<16) return std::nullopt;
  auto dig=[&](size_t i)->int{ char c=s[i]; if(c<'0'||c>'9') return -1; return c-'0'; };
  auto two=[&](size_t i)->int{ int a=dig(i), b=dig(i+1); if(a<0||b<0) return -1; return a*10+b; };
  if(dig(0)<0||dig(1)<0||dig(2)<0||dig(3)<0||s[4]!='-'||s[7]!='-'||s[10]!=' '||s[13]!=':')
    return std::nullopt;
  int y=dig(0)*1000+dig(1)*100+dig(2)*10+dig(3);
  int mo=two(5), d=two(8), h=two(11), mi=two(14);
  if(y<2000||y>2100||mo<1||mo>12||h>23||mi>59) return std::nullopt;
  static const int kMd[]{31,28,31,30,31,30,31,31,30,31,30,31};
  int dim=kMd[mo-1];
  if(mo==2 && ((y%4==0&&y%100!=0)||y%400==0)) dim=29;
  if(d<1||d>dim) return std::nullopt;
  size_t p=16;
  if(p<s.size() && s[p]==':'){ // detik opsional
    int sec=two(17); if(sec>59) return std::nullopt;
    p=19;
  }
  if(p!=s.size()) return std::nullopt; // full-consumption — sampah ditolak
  long long total=days_from_civil(y,(unsigned)mo,(unsigned)d)*1440LL + h*60LL + mi - 7LL*60;
  long long days=total/1440; long long rem=total%1440;
  if(rem<0){ rem+=1440; days-=1; }
  int y2; unsigned m2,d2;
  civil_from_days((int)days,y2,m2,d2);
  int hh=(int)(rem/60), mm=(int)(rem%60);
  char buf[64];
  snprintf(buf,sizeof(buf),"%04d-%02u-%02uT%02d:%02d:00Z",y2,m2,d2,hh,mm);
  return std::string(buf);
}

} // namespace examvan::helpers
