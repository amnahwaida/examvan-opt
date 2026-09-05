# Review Menyeluruh Seluruh Alur — Temuan 2026-09-06

Status: **14th review pass** (2026-09-06), setelah 13 pass sebelumnya mengeraskan
public auth, hasil, admin/scoring, WS, dan export. Pass ini = fan-out 4 subagent
(public / auth / admin / ws+queue+infra) + verifikasi independen terhadap
referensi Go beku (`EXAMVAN/webui`, commit terakhir) dan source uWS v20.71.

- Test suite: **679/680 hijau** (1 pre-existing skip `P7_Frontend.JsGuardCount`).
- Branch: 34 commit di depan `origin/main` (HEAD `1124fca`).
- **Lampiran A** (agent admin, terverifikasi 04:00): C7 (IDOR exam lintas
  instansi), C8 (IDOR submission/pengawas — PII + delete/export), M13 (voucher
  TOCTOU), M14 (pagination overflow admin), M15 (`save_packages` zeroing), dan
  catatan admin.
- **Konteks penting**: produksi kini **C++ murni** (Go `examvan-go-shadow`
  dihapus 2026-08-26, lihat `MIGRASI_STATUS.md`); nginx dual-run memungkinkan
  rollback per-grup ke Go (`nginx/nginx.conf` map, `docs-cutover.md`). Gap
  paritas di bawah karena itu **live di produksi**, bukan dev-only. Sebagian
  besar gap ini **tidak pernah tercakup parity F8** — shadow parity hanya
  mencakup `/api/health`, `/api/time`, `/api/exams` (JSON), `/hasil`,
  `/download`.

Tingkatan: 🔴 critical · 🟠 high · 🟡 medium · ⚪ info/clean.

---

## 🔴 C1. `/api/exams` (list_exams) — bocorkan token + `file_path`, tanpa scope instansi

**Lokasi**: `src/handlers/api/exams.cpp:268-285` (response JSON tiap item),
route `src/http/router_full.cpp:177` (tanpa auth).

**Bug**: endpoint publik mengembalikan `token`, `active_token`, dan `file_path`
untuk **semua ujian dari semua sekolah** ke siapa pun tanpa autentikasi maupun
parameter scope.

**Paritas Go** (`EXAMVAN/webui/internal/handlers/api/exams.go:287` `ListExams`):
- Mewajibkan query `instansi` (alias `kode`/`code`). **Kosong → daftar kosong**
  — guard cross-tenant eksplisit: "Without a code we return an EMPTY list
  rather than every school's active exams (which would be a cross-tenant leak)."
- Response item hanya: `id, name, status, size_mb, start_time, end_time,
  created_at`. **Tidak pernah** `token`/`active_token`/`file_path`.

**Dampak**: (a) token ujian (kunci akses siswa + pintu ke endpoint token-mode)
bocor massal; (b) `file_path` + `id` memberi petunjuk objek R2; (c) **tidak ada
pemisahan tenant** sama sekali — store C++ tidak punya kolom/scope `instansi`
(`exam_store_postgres.cpp` query global `list_all()`), jadi ini gap
multi-tenant fundamental, bukan sekadar bentuk respons.

---

## 🔴 C2. `/api/exams/:exam_id/pdf` — TANPA gate autentikasi/approval

**Lokasi**: `src/handlers/api/exams.cpp:611-630`.

**Bug**: cukup `exam_id` numerik + exam ada + `file_path` tidak kosong →
302 presigned R2. Tidak ada cek token, status aktif, jadwal, maupun approval
device. **Naskah soal dapat diunduh tanpa kredensial apa pun** oleh siapa pun
yang tahu/tebak `exam_id`, dan siswa bisa menarik PDF lebih awal tanpa melewati
gate persetujuan pengawas.

**Paritas Go** (`exams.go:768` `ExamPDF`):
1. `X-Exam-Token` wajib — kosong → 401 "Token tidak disertakan".
2. Exam harus ada + `IsActive()` + `ExamStartedAt != nil` + `examtoken.Matches`.
3. `ExamScheduleEnded` → 403.
4. **Gate approval device server-side**: `X-Device-Id` header (atau query
   `mac_address`) wajib punya baris `status='approved'` di `exam_approvals` →
   selain itu 403 "Perangkat belum disetujui pengawas". Komentar Go eksplisit:
   token statis dipakai sekelas, sehingga token saja TIDAK cukup — approval
   wajib dicek di server.
5. Rate limit per exam+device (`pdfRateLimitMax`).

**Catatan**: gate approval ini yang membuat flow siswa (request-approval →
pengawas setujui → baru bisa buka naskah) punya makna; tanpa gate di sisi
server, approval hanya hiasan UI.

---

## 🔴 C3. `/api/exams/:exam_id/result` — stub, poll Android tidak pernah selesai

**Lokasi**: `src/handlers/api/exams.cpp:711-750`; protobuf shape di
`proto/examvan.proto:358` `ExamResultResponse`.

**Bug**: selalu balas `{"success":true,"exam_id":N,"score":null,"has_score":false}`
(JSON) / `success=true` (protobuf). Tidak membaca hasil worker dari Redis,
tidak fallback ke DB, tidak ada access gate, tidak ada rate limit, tidak ada
field `status`. **Aplikasi Android mem-poll endpoint ini setelah submit**
(`EXAMVAN/android/.../api/ApiClient.kt:610-670` `getExamResult`, parse
`status` = `pending`/`done`/`failed` + `score`); dengan C++ poll selalu
"pending" → layar "jawaban diproses" tidak pernah selesai.

**Paritas Go** (`exams.go:1258` `ExamResult`):
1. Rate limit per exam+MAC dan per-exam (aggregate).
2. **Access gate**: `X-Exam-Token` cocok ATAU device masih `approved`
   (poll yang jatuh antara submit dan revoke pasca-commit / rotasi token).
   Tanpa kredensial → 401. Komentar Go: endpoint ini TIDAK boleh publik —
   tanpa gate ia membocorkan skor + tautan identitas siswa.
3. Baca Redis `queue.ResultKeyPrefix+jobID` → `status:"done"/"failed"` + score.
4. Fallback: `GetLatestSubmissionByIdentity` (DB) bila key Redis kedaluwarsa;
   butuh `job_id` (rahasia per-submission) + `mac_address`.
5. Belum ada apa-apa → `status:"pending"`.

---

## 🔴 C4. `submit_exam` — respons tidak menyertakan `job_id` + `congrats_message`; `start_time` hilang

**Lokasi**: `src/handlers/api/exams.cpp:632-709`.

**Bug**:
1. Respons 202 hanya `{"success":true,"status":"queued"}` — tanpa `job_id`.
   Android membaca `job_id` dari respons submit (`ApiClient.kt:585`) untuk
   poll `/result`; tanpa id, poll tidak bisa mencocokkan hasil worker.
2. Tidak ada `congrats_message` (Go sertakan dari `exam.CongratsMessage`).
3. `job.start_time` tidak pernah diisi dari body (`exams.cpp:679-686` hanya
   student_name/exam_number/student_class/mac/answers/identity_data) padahal
   worker meng-INSERT `start_time` ke `submissions.start_time`
   (`submission_queue.cpp:334`) → kolom waktu mulai siswa kosong.

**Paritas Go** (`exams.go:1123-1132`): respons 202 =
`{success, message:"Jawaban berhasil dikirim", status:"queued", job_id,
 score:null, congrats_message}`. `startTime := sanitizeStartTime(body.StartTime)`
dipakai di job.

**Dampak**: terkait erat C3 — submit dan poll adalah satu handshake async;
keduanya pincang di C++ sehingga alur submit async (jalur default saat Redis
ada) tidak bisa diselesaikan klien.

---

## 🔴 C5. Admin API — TANPA CSRF (frontend mengirim token, backend tak pernah cek)

**Lokasi**: wrapper `admin_api` `src/http/router_full.cpp:42-119`; route
`/admin/api/*` di `router_full.cpp:231-291`.

**Bug**: `admin_api` = cookie session → revalidasi PG → role gate → body limit
5MB → lanjut. **Tidak ada verifikasi `X-CSRF-Token`** untuk mutasi apa pun
(users/vouchers/settings/exams/submissions/pengawas).

**Paritas Go** (`EXAMVAN/webui/cmd/server/main.go`):
- `adminAPI := r.Group("/admin/api", middleware.AuthRequired())` (baris ~664).
- `csrfAPI := lockedAPI.Group("", middleware.CSRFRequired())` (baris ~689) —
  **semua** POST admin (exams, pengawas, submissions, users, settings,
  vouchers) di bawahnya. `middleware/csrf.go:42` `CSRFRequired` memvalidasi
  header `X-CSRF-Token` terhadap token sesi; GET/HEAD/OPTIONS/TRACE dikecualikan.

**Bukti frontend**: `static/js/admin-core.js:80-83` — `apiFetch` **mengirim
`X-CSRF-Token`** pada setiap POST/PUT/DELETE/PATCH. Token itu tidak pernah
divalidasi C++.

**Asimetri**: `/logout` CSRF-nya ketat (`logout.cpp:19-26`), tapi mutasi admin
yang jauh lebih sensitif tidak. SameSite=Lax meredam sebagian serangan
cross-site biasa, namun tidak menutup subdomain/XSS/lax-post-timeout — dan ini
gap paritas eksplisit terhadap desain Go.

---

## 🔴 C6. Layout key R2 divergen — PDF era Go tidak terbaca C++; rollback ikut rusak

**Lokasi**: `src/handlers/r2/r2.cpp:101-103` (`object_key_for_exam` =
`"exams/{id}/{filename}"`), dipakai konsisten di upload/verify/delete
(`src/handlers/admin/exams.cpp:412,466,470,753,843,1175`) dan serve
(`src/handlers/api/exams.cpp:624`).

**Paritas Go**: **`pdfs/{file_path}`** — admin upload `fmt.Sprintf("pdfs/%s",
filename)` (`EXAMVAN/webui/internal/handlers/admin/exams.go:346`), edit/delete
`pdfs/%s` (`:546,772`), student serve `pdfs/%s` (`api/exams.go:856`). Go juga
menyimpan `file_path` berformat `20060102_150405_<random>_<name>.pdf`
(`admin/exams.go:118-126 newExamObjectName`), sedangkan C++ menyimpan nama
bersih `[a-zA-Z0-9._-]+.pdf` (`src/handlers/admin/exams.cpp:170-185`).

**Dampak**: key C++ **konsisten internal** (ditest: `test_r2.cpp:15`,
`test_exam_production_tdd.cpp:224`; proto doc `ARCHITECTURE_...:131` menulis
"R2 key exams/<id>/<filename>") — tapi **tidak lintas-stack**. DB produksi
sudah berisi `file_path` format Go dan objek R2 lama ada di prefix `pdfs/`.
(a) Semua PDF eksisting **404** saat diserve C++ (key `exams/{id}/{file_path}`
tidak ada). (b) Rollback per-grup ke Go (`docs-cutover.md` / nginx map) akan
membuat ujian baru buatan C++ (di `exams/...`) **tidak terbaca Go**. Dok
cutover mengklaim "R2 SAMA / data tunggal" (`migrasi-cpp/05-...:11`) — klaim
itu salah untuk objek PDF. Perlu migrasi re-key atau baca adapter dua prefix.

---

## 🟠 H1. Limit body 5MB uWS (commit `e163d1f`) — `res->end()` ganda + bocor body antar-request

**Lokasi**: `src/server/server.cpp:334` (`g_uWS_body` thread_local),
`:406-416` (onData + 413 early-return), `:401-405` (onAborted).

**Bug**: pada onData, bila `g_uWS_body > 5MB` → `clear()` + `writeStatus("413")`
+ `end("payload too large")` + `return`. Tapi **uWS v20 tidak berhenti
mengantar `onData` setelah `end()`**. Verifikasi source uWS v20.71:
- `HttpResponse.h` `internalEnd`/`end` TIDAK men-null `inStream`.
- `HttpResponseData.h` `markDone()` men-null `onAborted` + `onWritable`, tapi
  **tidak `inStream`**.
- `HttpContext.h`: pemanggil `inStream(data, fin)` hanya dicek pointer
  `inStream` — **tanpa** gate `hasResponded()`/state.

Akibat: sisa body request yang sudah di-buffer (>5MB) tetap diantar ke handler
yang sama → kode 413 masuk lagi → `end()` **kedua kali** (tulis body dobel /
offset membengkak = UB; di release bisa korup respons). Selain itu
`g_uWS_body` adalah satu variabel thread_local + uWS mem-buffer per-koneksi →
sisa body request pertama bisa bocor sebagai prefix body request berikutnya
pada koneksi keep-alive yang sama (request desync). Handler harus punya flag
`responded` per-request, guard `hasResponded()`, dan setelah 413 menguras
sisa body atau `res->close()`.

**Catatan**: jalur posix sudah benar (`server.cpp:273-295`: 413 → tutup
koneksi). Regresi ini hanya di jalur uWS (produksi).

---

## 🟠 H2. Login `next` — CRLF → injeksi header `Location`

**Lokasi**: `src/handlers/auth/login.cpp:213-223`.

**Bug**: nilai `next` dari form/JSON di-`url_decode` dua kali lalu divalidasi:
`nx[0]=='/'`, `nx[1]!='/'`, tidak ada `\`, `%2f`, `%5c`, `//`, `..`, `:`.
Cek ini **tidak menangkap CR/LF yang sudah ter-decode** oleh `url_decode`
(`utils.cpp:71-88` mengubah `%0d`/`%0a` menjadi `\r`/`\n`). Nilai lolos
ditaruh verbatim ke header `Location`.

**Verifikasi uWS**: `writeHeader` (`HttpResponse.h` v20.71) menulis key+value
byte-for-byte, **tanpa strip CR/LF** — `Super::write(value...)`. Jalur posix
(`server.cpp:55`) juga menulis header mentah.

**Dampak**: `next=/%0d%0aSet-Cookie:examvan_session%3d<attacker>` → setelah
login sukses (303 + Set-Cookie session), browser korban menerima header
`Set-Cookie` tambahan dari attacker — overwrite cookie sesi/CSRF di domain
korban + injeksi header umum.

---

## 🟡 M1. Stub protobuf admin "berbohong" (list kosong / delete no-op success)

**Lokasi**: `src/handlers/admin/users.cpp:183-190` (`list_users` → total 0),
`:470-477` (`delete_user` → `success:true` TANPA menghapus),
`src/handlers/admin/submissions.cpp:85-90` (`list_submissions` → total 0),
pola sama di banyak handler admin lain (dashboard/exams/pengawas/vouchers).

**Bug**: tiap handler menaruh blok `#ifdef HAS_PROTOBUF
if(is_protobuf_accept(req)){ ...return; }` di **awal fungsi**, sebelum kerja
nyata. Klien dengan `Accept: application/x-protobuf` menerima respons stub —
untuk `delete_user` berupa "sukses" padahal **tidak ada yang dihapus**; untuk
list berupa daftar kosong. (Bandingkan `create_exam` `exams.cpp:477-488` yang
menaruh blok protobuf SETELAH kerja nyata — pola yang benar.)

**Dampak**: saluran protobuf (didokumentasikan di `ARCHITECTURE_CPP_UWS_PROTOBUF.md`,
`PROTOBUF_MANDATORY` bila 1) memberi hasil salah secara diam-diam. UI web tidak
terkena (fetch default `Accept: */*`). Severity medium karena butuh klien yang
sengaja meminta protobuf, tapi ini jebakan laten.

---

## 🟡 M2. Heartbeat flusher — poison-loop bila FK exam hilang

**Lokasi**: `src/queue/submission_queue.cpp:394-442` (`drain_heartbeat_batch`),
`drain_heartbeats_once` `:446-470`.

**Bug**: satu batch 500 payload di-RPOP, lalu `BEGIN` → insert per payload →
`COMMIT`. `parse_heartbeat_payload` hanya menolak JSON malformed; payload
**well-formed** dengan `exam_id` yang sudah tidak ada → **FK violation**
(`student_access_logs.exam_id NOT NULL REFERENCES exams(id)`, schema Go
`schema.sql:135`). PG masuk status "current transaction is aborted" → semua
statement berikutnya gagal → `COMMIT` gagal → **seluruh batch 500 di-requeue**
(`LPUSH`) → diulang tiap 30 detik selamanya (poison-loop). Satu payload buruk
mengunci antrean heartbeat.

**Pembanding**: worker submission mengabaikan error per-row (at-most-once,
`:326-337`) sehingga tidak kena loop; flusher heartbeat at-least-once malah
rawan.

---

## 🟡 M3. Duplikasi baris `submissions` (placeholder heartbeat vs submission nyata)

**Lokasi**: `src/queue/submission_queue.cpp:306-345` (worker `run_batch` INSERT
`... ON CONFLICT DO NOTHING`), placeholder insert flusher `:431-437`.

**Bug**: skema `submissions` (Go `schema.sql:107-119`) **tidak punya unique
constraint** apa pun, jadi `ON CONFLICT DO NOTHING` tidak pernah menahan apa pun
— worker **selalu INSERT baris baru**. Sementara flusher heartbeat membuat
"placeholder row" (answers NULL) untuk siswa yang join (`:431-437`). Hasilnya:
siswa yang heartbeat-join LALU submit punya **dua baris** (placeholder kosong +
submission nyata). Baris placeholder bocor ke daftar admin (`submissions.cpp:116`
tidak memfilter answers NULL).

**Paritas Go**: `upsertSubmissionRow` (`EXAMVAN/webui/internal/queue/submission_queue.go`)
— UPDATE baris terakhir untuk (exam, device, exam_number) apa pun isinya
(placeholder ATAU sudah submit) memakai `pg_advisory_xact_lock`, baru INSERT
bila tak ada. Komentar Go eksplisit: tanpa itu, retry/submit kedua membuat
"second row — the same student appeared twice in the admin monitoring table".

---

## 🟡 M4. OTP compare non-constant-time

**Lokasi**: `src/handlers/auth/register.cpp:323` (`if(otp!=u.otp_code)`),
`src/handlers/auth/recovery.cpp:213` — `std::string::operator!=` early-exit.

**Bug**: pembandingan OTP 6 digit tidak constant-time, padahal kode ini
memakai CT-compare untuk HMAC session (`cookie.cpp:70-72`), CSRF
(`csrf.cpp:22-24`), bcrypt (`password.cpp:39-41`). Side channel timing per
posisi pada ruang 10^6. Lockout 5× (`register.cpp:325`) dan rate limit
memperlambat tapi tidak menutup kanal.

---

## 🟡 M5. CSRF double-submit dilemahkan — body diparsing form/JSON tanpa cek Content-Type

**Lokasi**: `src/helpers/utils.cpp:90-106` (`parse_form` mem-parsing apa pun
sebagai `k=v&...`), `src/handlers/auth/auth_helpers.cpp:62`
(`request_csrf_token` → `json_field(req.body,...)`), dipakai semua handler auth
public (login/register/confirm/resend/forgot/reset).

**Bug**: tidak ada pemeriksaan `Content-Type`. Body yang ambigu (bisa dibaca
sebagai form maupun JSON) membuat ekstraksi token CSRF bisa membaca nilai dari
gaya form yang dikontrol attacker halaman. Skema double-submit (cookie ↔ body)
terdegradasi: token "cocok dengan cookie" bisa dipalsukan oleh siapa pun yang
mengontrol halaman → CSRF bypass pada mutasi auth public. (SameSite=Lax
meredam sebagian.)

---

## 🟡 M6. Race confirm/resend — lockout 5× OTP bisa diputar balik

**Lokasi**: `src/handlers/auth/register.cpp:323-331` (baca `otp_attempts` lalu
`bump_otp_attempts`), `register.cpp:368` (`resend_otp` →
`update_user_otp`), `auth_store.cpp:64-72` & `:300` (update OTP me-reset
`otp_attempts` ke 0).

**Bug**: interleaving `POST /register/confirm` (4× salah) dan
`POST /register/resend` me-reset `otp_attempts` ke 0 → percobaan tak terbatas
per jendela cooldown 60 detik. Non-atomic read-modify-write di PG. Fix yang
lebih aman: `UPDATE ... SET otp_attempts = otp_attempts + 1` dan cek nilai
setelah update.

---

## 🟡 M7. WS upgrade gagal jatuh ke stub `GET /ws/:room_id` tanpa cek Origin

**Lokasi**: `src/server/server.cpp:354-438` (`g_app->any("/*", ...)`),
`src/http/handlers.cpp:80-87` (stub `GET /ws/:room_id`).

**Bug**: upgrade WS yang sah dikonsumsi `g_app->ws(...)` (`:440-482`), tapi
permintaan upgrade yang **ditolak uWS sebelum handler WS** (key
`Sec-WebSocket-Key` hilang, header `Connection` tidak memuat `upgrade`, path
bukan `/ws/:room_id`, preflight) jatuh ke `any("/*")` → dispatch Router →
stub `GET /ws/:room_id` → respons `{"upgrade":"websocket","room":"..."}`
**tanpa cek Origin** (hanya `g_app->ws` upgrade yang cek origin di `:453`).
Room dari path (sudah di-decode, bisa berisi `"`) direfleksikan dalam JSON →
oracle enumerasi room lintas-origin + refleksi. Commit `1124fca` hanya
memperbaiki escaping JSON, bukan keberadaan stub. Di produksi stub ini mati
untuk upgrade sah tapi masih hidup untuk HTTP GET biasa dan upgrade gagal;
sebaiknya dihapus atau diberi gate.

---

## 🟡 M8. WS protobuf `pong` dikirim sebagai frame TEXT

**Lokasi**: `src/server/server.cpp:471-475` (uWS: `ws->send(m, TEXT)`),
jalur posix `flush_queue` (server.cpp `:121-131`).

**Bug**: balasan protobuf (ping → pong) di-queue ke `send_queue` lalu dikirim
sebagai frame TEXT di kedua jalur. Klien protobuf mengharapkan BINARY
(content-type `application/x-protobuf`). Ping/pong + heartbeat protobuf bisa
gagal interop. Medium (interop, bukan memory safety).

---

## 🟡 M9. `exam_by_token` — hilang cek jadwal + shape respons tidak cocok model Android

**Lokasi**: `src/handlers/api/exams.cpp:517-609`.

**Bug vs Go** (`exams.go:646` `ExamByToken`):
- Go cek `IsActive + ExamStartedAt` AND `ExamScheduleEnded` → 403. C++ hanya
  cek active/started (`:585`), **tidak cek ended** — siswa bisa join lewat
  token setelah waktu habis.
- Go balas objek exam lengkap: `id, name, status, security_level, strict_mode,
  public_results, show_answers, identity_fields, panel_color, size_mb,
  time_limit, start_time, end_time` + `questions` **dengan answer key
  di-strip** (`stripAnswerKeys`, `:1691`). C++ hanya `{token, status, id,
  name, success}` — model Android `TokenExamResponse` (`Exam.kt:44-50`,
  `Exam` fields `security_level`, `strict_mode`, `identity_fields`,
  `panel_color`, `questions`) tidak terisi.
- Go rate-limit join per-token; C++ tidak.
- C++ tidak strip `key`/`answer` — kalau suatu saat questions ikut dikirim,
  kunci jawaban bocor.

---

## 🟡 M10. `presign_url` tanpa fail-closed endpoint check

**Lokasi**: `src/handlers/r2/r2.cpp:41-99` (`presign_url` cek hanya
`enabled()` + bucket non-kosong).

**Bug**: upload/verify/remove memakai fail-closed `endpoint` harus mengandung
`r2.cloudflarestorage.com` (`r2.cpp:195,250,285`), tapi `presign_url` tidak.
Endpoint salah konfigurasi → URL presigned diam-diam menunjuk host lain.
`expires_seconds` juga tidak divalidasi (negatif/raksasa). Caller saat ini
memakai 3600 tetap (`api/exams.cpp:625`), jadi dampak rendah tapi inkonsisten
dengan pola fail-closed.

---

## 🟡 M11. Router — param segment di-URL-decode dua kali

**Lokasi**: `src/http/router.cpp:41` — `helpers::url_decode(ap[i])` di mana
`ap` sudah hasil decode path (`match` `:32` mendecode `path` dulu, lalu tiap
segmen didecode lagi).

**Bug**: nilai param (`room_id`, `token`, `exam_id`, dst) ter-decode ganda.
`%252F` → `/`. Saat ini tidak eksploitatif karena handler mem-`stoi`/validasi
charset, tapi jebakan untuk handler masa depan yang memakai param sebagai
filename/key. Token yang sah (alnum) tidak terpengaruh; room dengan `%2F`
berubah makna.

---

## 🟡 M12. Webhook WhatsApp tanpa signature — **paritas Go, bukan regresi**

**Lokasi**: `src/handlers/api/webhook.cpp:99-173`.

**Status**: C++ meniru Go persis (`EXAMVAN/webui/internal/handlers/api/webhook.go:15`
`WhatsappWebhook`) — Go juga tidak verifikasi signature. Proteksinya: OTP
(6 digit, 15 menit) + pencocokan nomor HP pengirim dengan `whatsapp_number`
terdaftar. Attacker butuh OTP DAN nomor terdaftar → bukan kebocoran praktis.
Dicatat sebagai info (bila mau diperketat, tambahkan shared-secret/verifikasi
di depan proxy Fonnte).

---

## ⚪ Info / bukan bug (verified clean)

- **Session cookie**: HMAC-SHA256, CT-compare, dual-key rotation, HttpOnly +
  SameSite=Lax + Secure (non-dev), Max-Age 24 jam; suspensi ditegakkan
  per-request via revalidasi PG admin_api.
- **bcrypt** `$2b$12$` CT-verify; batas 8–72 byte konsisten.
- **Semua query PG parameterized** (`exec_params`) di auth_store, admin,
  public, jobs — tidak ada SQL injection lewat concatenation.
- **Template HTML**: semua nilai user di-`html_escape`; username dibatasi
  `[a-z0-9._]`; SMTP header CRLF di-strip.
- **Role gate admin** (prior `3a01e73`): users/vouchers/settings/packages/
  saas/system-apps = superadmin. Exam/submission/pengawas tanpa role_req =
  **paritas Go** (guru/pengawas aktif boleh kelola exam — lihat `main.go`
  csrfAPI yang tidak SuperAdmin-gated untuk exam).
- **`change_password`** = operasi mandiri pada session id, verifikasi password
  lama — aman. Gate superadmin di C++ lebih ketat dari Go (management), bukan
  celah.
- **Router first-match**: `/admin` sebelum `/:token`, `submissions/export`
  sebelum `:id/detail` — benar.
- **WS fragment bounds** (231fc28) bertahan: posix 5MB kumulatif, uWS
  maxPayload 64KB / idle 120s / backpressure 1MB.
- **Rate limiter evict stale** (466fa5d); `g_admin_rl` key `X-Real-IP` yang
  di-overwrite nginx — aman di topologi terpasang.
- **Scoring**: satu sumber kebenaran `evaluate_question_detail` (d7b9911);
  clamp negatif; tanpa division-by-zero; konsisten export.
- **`strip_sensitive_keys`** JSON-aware (8689d1c); **hasil** rate-limited
  30/mnt; **download/app/:id** numerik-only (d21d810).
- **Commit `050ad58`** (non-root Docker): benar — `useradd` tersedia di
  bookworm-slim, server tidak menulis file runtime.
- **Commit `1124fca`**: escaping `json_string` benar.
- UWS onData 413 (H1): jalur posix sudah benar.

---

## Catatan lintas-dokumen

- `docs/exam-persistence-gaps.md` item "PostgreSQL field-update coverage is
  incomplete — update() belum persist semua field" **sudah usang**: pass
  sebelumnya memverifikasi `ExamStorePostgres::update()` menulis ulang 23
  field mutable (id/created_by/created_at immutable). Sebaiknya ditandai
  resolved.
- Sebagian besar temuan pass ini (C1–C4, C6, H1) berada di luar cakupan parity
  F8 dan belum pernah diuji lintas-stack. Setelah diperbaiki, tambahkan
  endpoint-endpoint siswa (pdf/result/submit/list) ke `scripts/shadow_parity.py`
  dan uji terhadap DB/R2 produksi (staging) sebelum rilis.

## Susunan perbaikan yang disarankan

1. Gate `/api/exams/:id/pdf` (token + approved device + rate limit).
2. Implementasikan `/result` poll nyata + `job_id`/`congrats_message` di submit.
3. Instansi-scope + strip token/file_path dari `/api/exams`.
4. CSRF di `admin_api` (validasi `X-CSRF-Token` yang sudah dikirim frontend).
5. Rework 413 uWS (flag `responded` + drain/close).
6. Re-key/adapter R2 `pdfs/` ↔ `exams/{id}/` untuk data lama.
7. Medium: CRLF `next`, stub protobuf no-op, poison-loop heartbeat, dup rows,
   OTP CT-compare, dll.

---
*Dokumen temuan — bukan changelog. Belum ada perbaikan yang dilakukan pada
saat dokumen ini ditulis (HEAD `1124fca`, 2026-09-06).*

---

## Lampiran A — tambahan dari agent admin (selesai 03:52, terverifikasi 04:00)

Temuan di bawah keluar setelah badan dokumen di atas ditulis; semuanya sudah
diverifikasi terhadap referensi Go.

### 🔴 C7. IDOR exam — guru/pengawas instansi mana pun bisa baca/ubah/hapus SEMUA exam (termasuk kunci jawaban lintas sekolah)

**Lokasi**: `src/http/router_full.cpp:258-277` (route exam tanpa `role_req`
maupun scope), `src/store/exam_store_postgres.cpp:220-223` (`list_all()` =
`SELECT ... FROM exams ORDER BY id` — tanpa filter `created_by`/instansi),
`:232-243` (`update` by bare id), `:244` (`remove` = `DELETE ... WHERE id=$1`).
Handler: `list_admin_exams` (`exams.cpp:187`), `update_exam`/`delete_exam`/
`bulk_toggle_exams`/`bulk_delete_exams`/`get_exam_questions`/
`save_exam_questions`/`delegate_data`/`delegate_exam` (`exams.cpp:832,1040` dll).

**Paritas Go**:
- `checkExamOwnership` (`EXAMVAN/webui/internal/handlers/admin/helpers.go:280`)
  — superadmin → true; pemilik (`created_by == userID`) → true; delegated
  (`delegated_to == userID`) → true; operator → true HANYA bila instansi sama
  dan bukan `personal`/kosong (guard cross-tenant eksplisit); selain itu false.
  Dipanggil oleh `GetQuestions` (`admin/exams.go:1277` → 403), dan seluruh
  mutasi exam.
- Model `ListExams` scope instansi (`models/exam.go:243`): `e.created_by IN
  (SELECT id FROM admin_users WHERE instansi = $N)`. Go TIDAK punya
  `GET /admin/api/exams` global — daftar admin via SSR ber-scope.

**Skenario**: guru B (SMA X) login, `GET /admin/api/exams/42/questions` utk
exam 42 milik SMA Y → menerima `questions_json` lengkap termasuk `key`/`answer`
(`exams.cpp:1040`). Lalu `POST /admin/api/exams/42/delete` → `delete_exam`
(`exams.cpp:832`) menghapus objek R2 + baris. Tidak ada konfirmasi server-side
(hanya `confirm()` frontend).

**Mengapa bukan disengaja**: data model punya `created_by` (`exams.cpp:448-456`),
`delegated_to`, dan scope delegate-data by instansi (`exams.cpp:1234-1313`) —
akses store yang tak ber-scope bertentangan dengan desain itu.

### 🔴 C8. IDOR submission/pengawas — PII siswa lintas sekolah + delete/export

**Lokasi**: route `router_full.cpp:278-287`; `submissions.cpp:83-155`
(`list_submissions` tanpa filter sekolah/role), `:158-208`
(`submission_detail` — balas `answers_json` + `identity_data` utk id mana pun),
`:236-262` (`delete_submission` — hapus baris mana pun);
`export.cpp:150-185` (`WHERE ($1=0 OR s.exam_id=$1)` → `exam_id=0` = semua
submission semua instansi); `pengawas.cpp:217-287` (`pengawas_submissions`
terima `exam_id` mana pun tanpa cek keanggotaan `exam_pengawas`),
`:290-346` (`pending_approvals`), `:348-380` (`set_approval` setujui device
exam mana pun), `:409-437` (`set_auto_approve`).

**Paritas Go**: Go punya `checkSubmissionOwnership` (`admin/helpers.go:325` —
ambil `exam_id` dari submission, lalu `checkExamOwnership`). Kontras internal
C++: `pengawas_exams` MEMILIKI scope `e.id IN (SELECT exam_id FROM
exam_pengawas WHERE user_id=...)` (`pengawas.cpp:139-141`) — saudara
endpoint-nya tidak.

**Skenario**: pengawas exam 5 bisa `GET /admin/api/submissions/123/detail`
(submission sekolah lain → lembar jawaban + identity_data siswa), atau
`GET /admin/api/submissions/export?exam_id=0` → unduh PII seluruh tenant
(nama, nomor ujian, kelas, MAC) sebagai XLSX.

### 🟡 M13. Voucher redeem — TOCTOU double-spend (tanpa transaksi/lock)

**Lokasi**: `src/handlers/admin/vouchers.cpp:528-565`.

**Bug**: langkah 3 baca `vused`/`max_usage` lalu cek `vused>=vmax` (`:528`),
langkah 5 `UPDATE vouchers SET used_count=used_count+1` (`:554`) — tanpa
`WHERE used_count < max_usage`, tanpa `BEGIN`/`COMMIT`, tanpa `FOR UPDATE`.
Dua redeem konkuren utk voucher `max_usage=1` sama-sama lolos cek `:528` →
dua-duanya increment → voucher sekali pakai mengaktifkan dua akun. Bila INSERT
(`:556`) atau UPDATE user (`:561`) gagal setelah increment, `used_count`
terbakar tanpa redemption.

**Paritas Go**: `RedeemVoucherHandler` (`admin/vouchers.go:464`) memakai
transaksi + `SELECT ... FOR UPDATE` (`:532-541`) — lock baris voucher mencegah
dobel-klaim.

### 🟡 M14. Pagination overflow di semua endpoint admin (fix hanya di /api/exams)

**Lokasi**: `submissions.cpp:120`, `users.cpp:196,228`, `vouchers.cpp:139,410`,
`pengawas.cpp:154,255,317` — `std::to_string((page-1)*per_page)` aritmetika
`int`; `page` tidak di-upper-bound (hanya `page<1` di-clamp). Bandingkan
`api/exams.cpp:250` yang memakai `static_cast<int64_t>`.

**Skenario**: `?page=2147483647&per_page=200` → `(2147483646)*200` overflow
int32 → OFFSET negatif → error PG → handler balas daftar kosong (kontrak
pagination rusak). Pola sudah pernah diperbaiki sekali di codebase (`dc10b93`)
tapi tidak disebar ke endpoint admin.

### 🟡 M15. `save_packages` meng-zero semua limit numerik paket (frontend kirim angka tanpa quote)

**Lokasi**: `src/handlers/admin/vouchers.cpp:691-695` —
`atoll(json_string_field(o,"max_exams"))`; frontend `settings-packages.js:88-93`
mengirim `{max_exams: 5, ...}` **tanpa quote** → `json_string_field`
(`vouchers.cpp:49-65`) mengharuskan `"`, balas `""` → `atoll("")=0`.
Respons tetap `{"success":true}` (`:713`) → korupsi diam-diam: tiap simpan
paket me-reset semua limit ke 0. (`label`/`key`/`role` string lolos, jadi baris
tetap "tersimpan".)

### ⚪ Info admin (bukan bug / verified clean)

- `X-Internal-Admin-Id` selalu di-overwrite dari session terverifikasi
  (`router_full.cpp:115-117`) — id dari klien tidak pernah dipakai.
- `delete_user`/`toggle`/`verify` menjaga `username=="superadmin"`.
- Export XLSX `inlineStr` → tanpa formula injection; CSV guard `'`; nama file
  statis → tanpa header injection.
- Semua query admin parameterized; fragment SQL dibangun dari allowlist
  (`sort_by`/`sort_dir`).
- `edit-pdf` filename TIDAK di-sanitasi (beda dgn create `exams.cpp:354` vs
  `:753,803`) + objek R2 lama tak dihapus saat ganti PDF — LOW, catat saja.
