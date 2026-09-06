# Review Menyeluruh Seluruh Alur — Temuan Pass-16 (2026-09-06)

Status: **remediation in progress; current tree has a verified green full suite after the first remediation batches**.

## Scope dan baseline

- Repository target: `/home/vannyezha/project/sekolah/examvan-opt`.
- Reference Go: `/home/vannyezha/project/sekolah/EXAMVAN/webui`, commit `13c05081bde133a6b681bed4865375b43ffd547f`.
- Baseline current test binary dari root: **681/682 passed**, 1 pre-existing skip: `P7_Frontend.JsGuardCount`.
- Current root `HEAD` includes build/docs commits `83e5729` and `bd9361c`; remediation edits are currently uncommitted.
- Uncommitted sebelum/selama review: `src/db/pool_real.hpp`, `src/handlers/auth/auth_store.cpp`, `src/server/server.cpp`, serta laporan pass-15 dan direktori `.claude/`. Perubahan kode tersebut bukan remediation pass-16 dan belum diaudit sebagai fix resmi.
- Beberapa subagent membaca worktree lama atau branch snapshot lama; klaim yang hanya berasal dari snapshot tersebut tidak dimasukkan. Temuan di bawah diverifikasi terhadap file current root atau merupakan temuan pass-15 yang kembali diverifikasi.

## Executive summary

Pass-14 dan pass-15 menutup beberapa exposure penting: PDF gate, result polling dasar, job ID submit, admin CSRF wrapper, R2 dual-layout, dan scope sebagian route per-ID. Namun current-tree review menunjukkan bahwa alur async submission masih memiliki masalah integritas serius: worker menerbitkan hasil sukses sebelum persistence PostgreSQL selesai, kegagalan enqueue Redis tidak dilaporkan ke klien, dan `job_id` result belum diikat ke exam/device/identity.

Temuan admin pass-15 juga masih terbuka: route list/bulk/export/dashboard lintas tenant, predicate pengawas/operator yang salah, session superadmin, assignment roster, voucher, settings, page CSRF, dan route/template parity.

## Prioritas remediasi

1. Hentikan false-success/loss pada submission queue dan pastikan enqueue/persistence durable sebelum `done`.
2. Ikat result `job_id` ke exam, device, identity, dan submission yang meminta result.
3. Tutup seluruh admin cross-tenant list/bulk/export/dashboard dan pisahkan access vs control authorization.
4. Perbaiki session superadmin dan validasi assignment/delegation.
5. Perbaiki voucher redeem/activate dan masked secret settings.
6. Tutup public protobuf token leak, approval flood, answer-key first-member, dan submit MAC bucket bypass.
7. Pulihkan route/template frontend yang hilang dan page CSRF/session revalidation.

---

## Temuan baru Pass-16 — HIGH / CRITICAL

### P16-C1. Worker menerbitkan hasil sukses sebelum persistence database

**Lokasi:** `src/queue/submission_queue.cpp:288-301,306-355`.

`run_worker()` mengambil job, menghitung score, memasukkannya ke `batch_q_`, lalu langsung memanggil:

```cpp
JobResult r{job->job_id, true, score, "ok", ...};
queue_->store_result(r);
```

Persistence PostgreSQL baru dilakukan kemudian oleh `run_batch()`. Semua return value `UPDATE`/`INSERT` pada `:335-355` diabaikan; tidak ada transaction/result check yang mengubah JobResult menjadi failed atau melakukan retry/requeue saat DB unavailable, constraint error, atau koneksi putus.

**Dampak:** client polling menerima `status=done`/score walau row submission tidak pernah tersimpan. Job Redis sudah diambil sehingga jawaban dapat hilang permanen. Ini merusak integritas nilai dan audit trail.

**Perbaikan:** persistence harus sukses dan commit dulu, baru publish result `done`; periksa seluruh SQL result, gunakan retry/backoff/requeue, dan publish `failed` hanya setelah retry policy berakhir.

### P16-C2. Submit selalu mengembalikan 202 walau enqueue Redis gagal

**Lokasi:** `src/handlers/api/exams.cpp:127-139,788-811`.

`enqueue_job_to_redis()` adalah `void` dan mengabaikan kegagalan koneksi Redis maupun `LPUSH`. `submit_exam()` tetap mengembalikan HTTP `202 queued` dan job ID.

**Dampak:** saat Redis down atau LPUSH gagal, client mengira jawaban sudah queued padahal payload tidak pernah masuk antrean. Digabung dengan P16-C1, alur async dapat kehilangan jawaban tanpa sinyal ke siswa.

**Perbaikan:** ubah enqueue menjadi `bool/Result`, verifikasi connection + LPUSH response, return 503/queue-unavailable jika gagal, dan jangan menerbitkan job ID yang tidak durable.

### P16-C3. Result `job_id` tidak diikat ke exam/device/identity

**Lokasi:** `src/handlers/api/exams.cpp:842-900` dan fallback `:902-927`.

Endpoint memproses `job_id` dari query caller lalu mengambil Redis key result tersebut. Isi result hanya dipindai untuk `success` dan `score`; tidak ada validasi bahwa payload result memiliki `job_id` yang sama, `exam_id` yang sama, MAC/device yang sama, atau identity yang sama dengan request. Gate hanya memvalidasi token/device untuk `exam_id` pada URL.

Fallback PostgreSQL juga mengambil submission terbaru berdasarkan `exam_id + mac_address` dan mengabaikan `job_id` serta `identity_data` (`(void)identity` di `:927`).

**Dampak:** pemegang token/device valid untuk satu exam yang mengetahui job ID submission lain dapat menerima score submission tersebut; pada shared device, fallback dapat mengembalikan nilai siswa lain.

**Perbaikan:** simpan binding `{job_id, exam_id, mac/device, identity fingerprint, submission_id}` pada job/result; validasi semua binding sebelum response; fallback harus mencari job/submission yang sama, bukan latest exam+MAC.

### P16-C4. Public protobuf `/api/exams` tetap membocorkan token

**Lokasi:** `src/handlers/api/exams.cpp:253-266`; route public `src/http/router_full.cpp:265-269`.

JSON list sudah membuang token, tetapi protobuf `ListExamsResponse` masih mengisi `tokens` dengan `active_token`/`token` untuk semua exam. Endpoint tidak authenticated dan belum punya tenant scope.

**Perbaikan:** protobuf harus memiliki metadata publik yang sama dengan JSON tanpa token/file path; wajibkan scope instansi/kode bila endpoint memang multi-tenant.

### P16-C5. Approval request dapat diflood tanpa rate limit

**Lokasi:** `src/handlers/api/exams.cpp:413-530`.

Tidak ada bucket per exam dan per device sebelum token validation/persistence. Dengan token exam yang bocor dan MAC unik, attacker dapat mengisi pending/approved rows; cap approved tidak menggantikan rate limit queue.

**Perbaikan:** tambahkan rate limit per exam dan per device, termasuk aggregate distributed-flood control, sebelum persist.

### P16-C6. Jalur protobuf submit tidak mengembalikan `job_id`

**Lokasi:** `proto/examvan.proto:351-356`; `src/handlers/api/exams.cpp:801-807`.

Respons JSON submit memuat `job_id`, tetapi `SubmitExamResponse` protobuf hanya memiliki `success`, `status`, `error`, dan `error_code`. Handler protobuf mengembalikan `202 queued` tanpa job identifier. Klien protobuf tidak dapat memanggil result polling untuk submission yang baru dikirim.

**Perbaikan:** tambahkan field `job_id` (dan `congrats_message` bila dibutuhkan kontrak) ke protobuf, regenerasi bindings, lalu uji submit→poll pada klien protobuf.

### P16-C7. Submit tidak memvalidasi required identity fields

**Lokasi:** `src/handlers/api/exams.cpp:763-786`.

Handler hanya menyalin `student_name`, `exam_number`, `student_class`, dan `identity_data` ke job. Tidak ada validasi terhadap `exam.identity_fields` dan field wajib sebelum submission diantrekan, sementara Go menolak identity yang required tetapi kosong.

**Dampak:** submission tanpa identitas wajib dapat masuk monitoring, membuat hasil ambigu atau tidak dapat dipetakan ke peserta.

**Perbaikan:** parse `identity_fields` exam, validasi field required setelah sanitasi, dan return 400 sebelum enqueue.

---

## Temuan baru Pass-16 — MEDIUM / LOW

### P16-M1. Submit rate limit dapat dibypass dengan MAC arbitrary

**Lokasi:** `src/handlers/api/exams.cpp:739-744`.

Rate-limit key memakai `sub_mac` mentah. Endpoint `access_log` di `:1013` menggunakan `sanitize_mac_like_go`, tetapi submit tidak melakukan sanitasi sebelum rate-limit maupun approval lookup.

**Dampak:** client dapat mengubah nilai MAC pada setiap request untuk membuat bucket berbeda dan melewati batas per-device.

**Perbaikan:** sanitize/canonicalize MAC/device sekali sebelum rate-limit, approval, dan job construction; reject empty/unknown pada jalur yang memerlukan device.

### P16-M2. Public answer-key stripping gagal bila field menjadi member pertama

**Lokasi:** `src/utils/sanitize.cpp:47-73`; digunakan pada `src/handlers/api/exams.cpp:630-640` dan `src/handlers/public/hasil.cpp:516-519`.

`strip_sensitive_keys()` hanya mencari `,"key":` dan `,"answer":`. Payload object seperti `{"key":"A",...}` tidak cocok karena tidak memiliki comma prefix.

**Perbaikan:** parse JSON terstruktur atau dukung object-start/nested object dengan parser yang benar; tambahkan regression test untuk key-first, answer-first, nested array, dan multiple-choice key array.

### P16-M3. Mandatory protobuf tidak diterapkan ke student routes

**Lokasi:** `require_protobuf()` hanya dipanggil pada `src/handlers/admin/exams.cpp:259-264`; student routes di `src/http/router_full.cpp:265-275` tidak dibungkus enforcement yang sama.

Jika `protobuf_mandatory` diaktifkan, admin create dapat mewajibkan protobuf tetapi submit/request-approval/access-log/complete masih menerima JSON.

**Perbaikan:** terapkan middleware enforcement secara konsisten di router atau wrapper API, dengan pengecualian route yang memang public JSON by contract.

### P16-M4. Worker shutdown meninggalkan batch queued

**Lokasi:** `src/queue/submission_queue.cpp:279-284,306-315`.

`Worker::stop()` mengubah `running_=false` lalu join. `run_batch()` memakai `while(running_)`; batch yang sudah berada di `batch_q_` dapat ditinggalkan saat shutdown tanpa persistence/requeue.

**Perbaikan:** drain batch sebelum join atau requeue seluruh in-flight job secara durable ketika shutdown.

### P16-M5. Token rotation tetap memakai in-process locking

**Lokasi:** `src/handlers/api/exams.cpp:572-597`.

Rotation `active_token` menggunakan lock store/in-process. Dengan dua instance C++ atau dual-run Go+C++, masing-masing dapat membaca/merotasi token berbeda dan last-write-wins.

**Perbaikan:** gunakan PostgreSQL row lock/transaction (`SELECT ... FOR UPDATE`) seperti Go `MaybeResetActiveToken`.

### P16-M6. Edit PDF filename belum disanitasi

**Lokasi:** `src/handlers/admin/exams.cpp:715-765`, terutama key `:753`.

Multipart `edit_pdf_name` dipakai langsung ke `object_key_for_exam()`, sedangkan create path melakukan sanitasi. Filename dengan separator/traversal-like value dapat menulis key R2 yang tidak konsisten dengan `file_path` dan meninggalkan object lama.

**Perbaikan:** basename + allowlist + force `.pdf`; cleanup old/new object secara compensating/transactional.

---

## Temuan Pass-15 yang masih terbuka — ADMIN

### P16-A1. Admin list/bulk/export/dashboard lintas tenant — HIGH

**Lokasi:** `src/http/router_full.cpp:349,354-355,369,371`; `src/handlers/admin/exams.cpp:186-224,1142-1192`; `src/handlers/admin/submissions.cpp:83-149`; `src/handlers/admin/export.cpp:150-184`; `src/handlers/admin/dashboard.cpp:82-155`.

Route list/bulk/export tanpa ID tidak melewati scope wrapper. `list_all()` mengembalikan exam global; bulk toggle/delete menerima arbitrary IDs; submission list/export bersifat global; dashboard merender token dan stats global.

**Perbaikan:** scope query dan setiap bulk ID dengan actor access policy; dashboard/stats hanya menghitung exam yang accessible.

### P16-A2. Predicate pengawas/operator vs control salah — HIGH

**Lokasi:** `src/http/router_full.cpp:142-163,356-380`; `src/handlers/admin/pengawas.cpp:215-406`; `src/handlers/admin/exams.cpp:1208-1343`.

Satu predicate owner/delegate/superadmin dipakai untuk semua route. Pengawas assigned via `exam_pengawas` dan operator same-instansi terblokir dari monitoring/approval; owner guru biasa dapat menjalankan delegation yang seharusnya operator/superadmin.

**Perbaikan:** pisahkan `UserCanAccessExam` dan `UserCanControlExam`; role-gate delegation.

### P16-A3. Session superadmin tidak membawa flag superadmin — HIGH

**Lokasi:** `src/handlers/auth/login.cpp:27-29,184-198`; `src/session/cookie.cpp:125-138`; `src/http/router_full.cpp:147-160`; `src/handlers/admin/users.cpp:309-323`.

Role disimpan sebagai `role=["superadmin"]`, tetapi `is_super_admin` tidak diserialisasikan dan parser hanya membaca field terpisah itu. Scope per-exam dan pembuatan operator dapat menolak superadmin nyata.

**Perbaikan:** canonical role parser dan satu predicate exact untuk superadmin.

### P16-A4. Assignment pengawas/delegation menerima ID arbitrary — HIGH

**Lokasi:** `src/handlers/admin/exams.cpp:991-1018,1066-1133,1327-1333`.

`pengawas_ids` hanya diparse sebagai integer; tidak ada validasi active/role/instansi. Available pengawas global. Hasil SQL transaction tidak diperiksa dan tidak ada rollback pada kegagalan parsial.

**Perbaikan:** validate target dalam transaction terhadap status, role, dan tenant; periksa semua SQL result.

### P16-A5. Voucher redeem selalu rollback — HIGH

**Lokasi:** `src/handlers/admin/vouchers.cpp:540-611`, terutama `:595-601`.

INSERT redemption tanpa `RETURNING` menghasilkan `PGRES_COMMAND_OK`, tetapi kode hanya menerima `PGRES_TUPLES_OK`, sehingga redeem normal rollback/500.

**Perbaikan:** cek status command yang benar atau tambahkan `RETURNING id`; integration test PostgreSQL.

### P16-A6. Voucher activation non-transaksional dan expiry tidak divalidasi — MEDIUM

**Lokasi:** `src/handlers/admin/vouchers.cpp:633-658`.

Tiga update tanpa transaction/rollback; query tidak memastikan `is_active`, `remaining_seconds > 0`, atau expiry. Redemption expired/zero/negative dapat diaktifkan kembali.

**Perbaikan:** transaction + row lock + validasi status/durasi/expiry.

### P16-A7. Arbitrary roles pada create/edit user — MEDIUM

**Lokasi:** `src/handlers/admin/users.cpp:288-323,383-448`.

Array `roles` disimpan tanpa allowlist meski singular `role` divalidasi. Payload `roles:["superadmin"]` dapat menciptakan akun privileged melalui route yang dapat dipanggil superadmin.

**Perbaikan:** canonical role allowlist, protect superadmin targets, audit role changes.

### P16-A8. Masked secrets dapat menimpa secret asli — MEDIUM

**Lokasi:** `src/handlers/admin/settings.cpp:162-197,228-263`; `static/js/admin.js:3633-3642,3893-3948`.

Frontend mengirim kembali `smtp_password`/`turnstile_secret_key` yang sudah dimask; backend melakukan upsert mentah.

**Perbaikan:** server menganggap mask sebagai unchanged atau frontend tidak mengirim field tersebut.

### P16-A9. Admin/page authorization fail-open — MEDIUM

**Lokasi:** `src/http/router_full.cpp:63-70,75-103,147-160,281-320`.

Direct signed-cookie branch tidak mewajibkan `admin_id>0`; configured DB yang unavailable menyebabkan `pg_up=false` dan request tetap lanjut; page route hanya signature check tanpa status revalidation.

**Perbaikan:** fail closed pada production jika DB configured tetapi unreachable; wajibkan positive admin ID; revalidate page routes.

### P16-A10. Pengawas detail/template/route/CSRF rusak — MEDIUM/LOW

**Lokasi:** `templates/admin/pengawas_detail.html:83,149,157,1094,2018-2020`; `src/handlers/admin/template_helper.cpp:6-26`; `src/handlers/admin/submissions.cpp:67-79`; `src/http/router_full.cpp:314-320,375-380`; `src/handlers/auth/logout.cpp:17-47`.

Tidak ada rendered pengawas detail template; fallback hanya mengganti version sehingga placeholder Go dikirim mentah. Audit endpoint tidak terdaftar. Submissions page memakai rendered HTML langsung tanpa CSRF refresh. Logout form masih menggunakan hidden token stale/hardcoded yang tidak cocok dengan cookie segar.

**Perbaikan:** port/render template dengan data C++, implement/remove audit route, gunakan `render_admin_page`, dan samakan logout token dengan cookie.

### P16-A11. Frontend route parity gaps — LOW/INFO

- `static/js/admin.js:3750` memanggil `/admin/api/saas-settings/test-smtp`, tetapi route tidak ada.
- `static/js/settings-system-apps.js:32,411,484` mengharapkan list/upload/delete, sedangkan `router_full.cpp:381-382` memetakan system-apps ke settings handlers umum.
- `static/js/uiux-batch6-jscore.test.mjs`/frontend mengharapkan `/admin/api/pengawas/state`, tetapi route tidak terdaftar.
- `pengawas_detail.html:1855` memanggil audit route yang tidak tersedia.

---

## Temuan Pass-15 yang masih terbuka — PUBLIC/AUTH/RESULT

### P16-P1. `/hasil` redirect CRLF — HIGH

**Lokasi:** `src/handlers/public/hasil.cpp:393-403`.

Query token didecode lalu dimasukkan mentah ke `Location` tanpa allowlist/encoding.

### P16-P2. Login timing enumeration dan tidak ada account lockout — MEDIUM

**Lokasi:** `src/handlers/auth/login.cpp:152-195`.

Unknown user tidak menjalankan dummy bcrypt dan tidak ada lockout per-account seperti Go.

### P16-P3. Login CSRF parser mengabaikan Content-Type — MEDIUM

**Lokasi:** `src/handlers/auth/login.cpp:106-124`.

Login masih mencoba parsing form dan JSON tanpa membatasi berdasarkan Content-Type.

### P16-P4. HTML admin session tidak direvalidasi — MEDIUM

**Lokasi:** `src/http/router_full.cpp:281-320`.

Signature cookie cukup untuk membuka page; suspended/deleted account dapat memakai page sampai expiry cookie.

### P16-P5. CSRF cookie bukan `__Host-` — MEDIUM

**Lokasi:** `src/handlers/auth/login.cpp:71-79`; `src/handlers/admin/template_helper.cpp:84-85`.

Subdomain dapat mencoba overwrite double-submit cookie; gunakan `__Host-` atau server-side session CSRF.

### P16-P6. Public result masih mengirim identity/correctness detail saat `show_answers=0` — MEDIUM/product policy

**Lokasi:** `src/handlers/public/hasil.cpp:555-590`.

C++ mengikuti Go saat ini, tetapi anonymous API menerima identity data dan `evaluated_answers` walau show_answers false. Putuskan apakah policy memang mengizinkan detail tersebut.

---

## Hal yang diverifikasi tetap baik

- Test baseline tetap 681/682 dengan satu skip pre-existing.
- PDF current handler memiliki token, active/started, schedule, approved-device, R2 configured, dan rate-limit gates (`api/exams.cpp:664-720`).
- Submit current handler melakukan exam/token checks dan membangun job dengan answers/identity/start time (`api/exams.cpp:722-811`), tetapi enqueue failure dan raw MAC tetap menjadi temuan P16-C2/P16-M1.
- Result memiliki access gate dan status pending/done/failed dasar, tetapi binding job dan fallback identity belum aman.
- Heartbeat helper hanya LPUSH untuk `event=="heartbeat"`; tidak ada duplicate login LPUSH pada current helper.
- Router parameter tidak lagi double URL-decode.
- R2 `presign_url` current code memakai region `auto` dan fail-closed endpoint check (`r2.cpp:27-48`); klaim region `us-east-1` tidak berlaku pada current root dan tidak dimasukkan.
- uWS current body state memakai per-request `UwsRequestState`, responded/aborted guard, dan close-after-413 (`server.cpp:402-454`).
- Protobuf WS queue mempertahankan binary flag dan uWS mengirim binary sesuai flag (`server.cpp:487-492`).

## Rekomendasi pengujian

1. Tambahkan integration test PostgreSQL+Redis untuk: Redis down saat submit, DB down setelah dequeue, SQL constraint failure, shutdown dengan batch pending, dan result cross-job.
2. Tambahkan tests yang memastikan Redis result menyimpan dan memvalidasi binding job/exam/device/identity.
3. Tambahkan JSON/protobuf parity tests untuk list exams dan mandatory protobuf mode.
4. Tambahkan tests key-first answer stripping serta raw-MAC submit rate-limit canonicalization.
5. Tambahkan admin matrix test: owner, delegate, assigned pengawas, operator same/different instansi, superadmin, suspended user, DB unavailable.
6. Tambahkan browser route test: login → dashboard → submissions → logout, pengawas detail, audit tab, system-apps, test SMTP.
7. Jalankan staging dual-instance token rotation dan R2 PDF compatibility.

---



## Remediation progress — 2026-09-06

Implemented and verified in the current tree:

- Queue workers no longer publish `done` before persistence; DB transactions check SQL results, retry/requeue failed jobs, and shutdown drains staged work. Redis enqueue failures now return a non-queued error instead of false `202`.
- Result records now carry binding metadata and `/result` validates job/exam/device; durable fallback queries exact `job_id`.
- `SubmitExamResponse` protobuf now includes `job_id` and `congrats_message`; generated bindings were rebuilt.
- Public protobuf exam listing no longer emits tokens. Submit MACs are canonicalized before rate limiting, answer-key stripping handles first-member keys, and approval requests have exam/device rate limits.
- Public results hide `identity_data`, `evaluated_answers`, and raw answers from anonymous viewers when `show_answers=false`, per product decision.
- Canonical role parsing/session superadmin derivation is implemented; signed legacy superadmin cookies remain compatible. Admin wrapper rejects non-positive IDs, uses exact role checks, and supports separate exam access scope for assigned pengawas/same-instansi operators. Delegation routes require operator control.
- Voucher redemption accepts the correct PostgreSQL command status; voucher activation uses transaction/row lock and rejects inactive/expired/zero-duration redemptions. Masked settings secrets are preserved, and user role arrays are allowlisted.
- Edit PDF filenames are sanitized using the create-upload policy; submissions pages refresh CSRF through the admin renderer; system-app routes no longer fall through to unrelated SaaS settings handlers.
- Admin bulk toggle/delete now apply owner/delegate filtering and bulk delete cleans both legacy/current R2 layouts; catch-all token redirects require the exam-token allowlist.
- Existing and newly adjusted tests remain green in focused runs; the full current suite is green at **681/682 passed, 1 pre-existing skip**.

Still open / requiring the next implementation batch:

- PostgreSQL-backed tenant filtering for admin list/submission/export/dashboard/bulk routes; current immediate list redaction/scope does not replace a complete instansi query policy.
- Full production PostgreSQL verification for pengawas roster/tenant queries and browser rendering of the audit tab.
- HTML page database status revalidation and logout/form token browser verification remain; login now has process-local lockout/dummy-bcrypt protection, login JSON/form CSRF parsing is separated, and production login/admin-page CSRF cookies use the `__Host-` prefix.
- Complete system-app CRUD, SMTP-test, and pengawas-state route implementations or explicit UI removal remain; the pengawas audit route is registered and implemented.
- PostgreSQL/Redis integration tests for durable queue commit/retry, result binding, voucher transactions, tenant authorization, and schema migrations. No finding is considered production-closed until these integration paths pass.

*The original findings above remain historical; this section records implementation status and does not erase unresolved findings.*
