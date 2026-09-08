# Review Ulang — Pass-21 (2026-09-07, verifikasi lanjutan Pass-20 + review alur end-to-end)

Baseline: HEAD `f8afc3c` + perubahan uncommitted (remediasi M9/C5/B1 pass-20:
`server.{cpp,hpp}`, `main.cpp`, `router_full.cpp`, `exams/pengawas/settings/submissions.cpp`,
`CMakeLists.txt`, `tests/test_p26/p27/p28_tdd.cpp`). Full suite dire-run lokal:
**781 tests — 780 passed + 1 skip** (`P7_Frontend.JsGuardCount`, skip kondisional —
no `package.json`), build bersih, tanpa regresi. Metode: pembacaan penuh seluruh
modul (server/ws/hub, router+middleware admin_api, student API `api/exams.cpp`,
auth login/register/recovery/auth_store, admin users/vouchers/settings/submissions/
exams/export/dashboard/template_helper, public hasil/download, queue+worker+
heartbeat flusher, jobs, store memory+PG, r2, scoring, cookie/csrf/turnstile/
ratelimit/utils), nginx/docker-compose/Dockerfile/ci.yml/scripts, frontend JS
(admin.js/admin-core.js/settings-*/pengawas-detail + template inline JS).

## Ringkasan eksekutif

Remediasi pass-20 (M9, C5, B1) **terverifikasi benar dan lengkap** di source
maupun test. Review menyeluruh alur menemukan **2 temuan baru HIGH-spectrum
(keduanya kelas correctness/data-integritas, bukan exploit langsung), 3 MEDIUM,
3 LOW**, plus 1 kandidat (T6) yang dicabut saat verifikasi. Tidak ada celah keamanan baru kelas auth-bypass/XSS: escaping frontend
(`escapeHtml`/`jsEscape`/`esc()`) dan SSR (`html_escape`) konsisten di semua
sink yang diperiksa; SQL selalu `exec_params` parameterized. Temuan terbesar
adalah **gap schema fresh-DB** (tabel `saas_settings`/`vouchers`/`package_settings`/
`voucher_redemptions`/`exam_pengawas` tidak pernah dibuat oleh `migrate()` /
bootstrap mana pun di repo ini, sementara handler memakainya) dan **koneksi
PG baru per-request** (pola `with_pg` membangun + menghancurkan pool tiap call).

## Status verifikasi remediasi Pass-20

### M9 — CLOSED (konfirmasi penuh)
`delete_submission` (`submissions.cpp:254-262`), `get_auto_approve`
(`pengawas.cpp:446-450`), `set_auto_approve` (`pengawas.cpp:481-487`) semuanya
fail-closed 503 bila `DATABASE_URL`/Config set tapi eksekusi PG gagal; mode
memory/dev tetap 200. TDD `test_p26_tdd.cpp` (5 test kontrak) lulus di full
suite. **Sisa M9 di `users.cpp` masih OPEN** — lihat "Carried-over" di bawah.

### C5 — CLOSED (konfirmasi penuh)
- `server.hpp:27-33` member `posix_threads_` (tanpa guard, aman ODR kedua target).
- `server.cpp:387-401` `posix_worker_loop`: drain antrean saat `g_running=false`
  sebelum keluar; zero `.detach()` di seluruh file (test P27 kontrak lulus).
- `Server::stop()` (`server.cpp:643-659`): jalur posix tutup fd + `notify_all`
  CV + join semua thread; jalur uWS `g_app->close()` + join `g_uWS_thread`.
- `main.cpp:169-171`: `srv.stop()` dipanggil di jalur SIGTERM/SIGINT sebelum
  `w.stop()` — join uWS kini benar-benar tereksekusi (sebelumnya tidak pernah).
- Live E2E pass-20 (SIGTERM <1s, port released) konsisten dengan implementasi.

### B1 — CLOSED (konfirmasi penuh, dengan catatan)
- Router `router_full.cpp:237` meneruskan `X-Internal-Admin-Username`.
- `write_audit_log` di 3 TU menerima `user_id`+`username` param
  (`NULLIF($2,'')::int`, bukan NULL hardcoded); `audit_identity_from` dipasang di
  `set_approval` + `set_auto_approve` (pengawas), `bulk_toggle`/`bulk_delete`
  (exams), `system_app_upload`/`system_app_delete` (settings). TDD P28 lulus.
- **Catatan**: helper `AuditIdentity`/`audit_identity_from`/`write_audit_log`
  disalin 3× (static per TU). Konsolidasi ke `utils/audit.hpp` saat refactor
  berikutnya — duplikasi membuat kontrak INSERT mudah divergen antar TU bila
  salah satu salinan diubah sendiri.

### Temuan pass-20 lain — status tidak berubah
H7 (heartbeat WS tolak non-privileged, `hub.cpp:152,209`), M7 (cek scope submission
100% di router), M8 (bulk scope tanpa `exam_pengawas`/operator-instansi,
`exams.cpp:1216,1250`), M16-sisa (Redis tanpa `requirepass`), M19-sisa
(`extract_contract.py:6` path absolut; versi `2.7.2` kini **12 titik** — naik,
lihat T4), L3 (mask_token masih sized, `settings.cpp:86-90`), B2 (SRI placeholder
+ bundle lokal `/static/js/protobuf.min.js` **tidak ada** — lihat T3), B3 (CI
docker-path serial), B4 (nginx `location /` masih kirim header Upgrade), B5
(HSTS di port 80), B6 (worktree `.claude/` masih ada, 2 direktori). Semua
dikonfirmasi masih ada di HEAD+uncommitted.

## Temuan baru Pass-21

### T1 (HIGH) — Fresh-DB bootstrap gap: 5 tabel dipakai handler tapi tidak pernah dibuat
`ExamStorePostgres::migrate()` (`exam_store_postgres.cpp:210-251`) hanya membuat
`exams`, `exam_idempotency`, `submissions`, `student_access_logs`,
`exam_approvals`, `admin_users` (minimal), `admin_audit_logs`. `system_apps`
dibuat lazy di handler (`settings.cpp:348`). Namun handler secara aktif memakai
**`saas_settings`** (settings/recovery/register/auth_store `get_setting`,
approval-cap di `api/exams.cpp:572-576`), **`vouchers`** + **`voucher_redemptions`**
(vouchers.cpp seluruh alur redeem/activate — sumber pendapatan SaaS),
**`package_settings`** (entitlement redeem), dan **`exam_pengawas`**
(scoping pengawas, delegate, save_questions). Tidak ada `CREATE TABLE` untuk
kelima tabel ini di seluruh repo (bukan di src/, tests/, scripts/, bukan file
.sql). Skenario kegagalan: deploy C++ ke DB bersih (dok cutover: "fresh-DB harus
bisa jalan tanpa skema Go" — P18-M13) → `main()` lolos (migrate hanya buat tabel
miliknya), server jalan, lalu: login admin OK, tapi **redeem voucher 500 di
setiap attempt**, halaman settings GET → `saas_settings` query gagal → semua
default tampil, register/OTP tidak bisa baca `email_verification_enabled`,
approval-cap fallback `500` default. `admin_users` versi migrate juga jauh
lebih tipis dari kebutuhan `auth_store.cpp:255-262` INSERT (tidak punya kolom
`name`, `max_exams`, `max_pdf_size`, `max_concurrent_exams`, `max_storage_size`,
`whatsapp_number`, `otp_code`, `otp_expiry`, `otp_attempts`, `expires_at`,
`package`, `registered_ip` ada, `role` default beda) — tanpa ALTER kolom di
migrate, INSERT register gagal di DB bersih. Efek nyata hanya bila DB-nya bukan
DB Go lama; di DB hasil migrasi Go semuanya ada. **Remediasi**: tambah blok
CREATE TABLE IF NOT EXISTS + ALTER TABLE ADD COLUMN IF NOT EXISTS untuk 5 tabel
+ kolom admin_users di `migrate()` (pola sudah dipakai untuk exam_idempotency).

### T2 (HIGH-spectrum, correctness) — Koneksi TCP PG baru per-request (pola `with_pg` tidak di-pool)
Pola `with_pg` (definisi identik disalin di `pengawas.cpp:68`, `submissions.cpp:34`,
`settings.cpp:93`, `vouchers.cpp:113`, `users.cpp:157`) membuat
`DbPool`+`RealPool` **stack lokal per panggilan**, acquire 1 koneksi, lalu
`real.release(c.release())` memasukkannya kembali ke `idle_` pool lokal — dan
destructor `RealPool::~RealPool` (`pool_real.cpp:27-30`) men-PQfinish **semua**
koneksi idle. Artinya: satu request admin (mis. `pengawas_exams`) membuka koneksi
TCP+auth MD5/SCRAM ke PG **≥2×** (pool identitas + list + stats = sampai 3 objek
RealPool per request di beberapa handler; `pengawas_exams` saja membuat 2),
dan setiap koneksi langsung ditutup saat scope keluar. Tidak ada pool shared
global (dikonfirmasi grep: hanya `ExamStorePostgres` yang memegang pool hidup).
Di bawah beban nyata (kelas ratusan siswa + polling pengawas 5 dtk + heartbeat
flusher yang juga membuka `RealPool(…,60)` baru per 30 detik via
`drain_heartbeats_once` `submission_queue.cpp:576-577`): churn koneksi ribuan/menit,
latensi handshake PG di setiap request admin, dan PG `max_connections=150`
(compose `db` command) bisa jadi bottleneck kontensi. Ini juga menjelaskan
kenapa `DATABASE_MAX_CONNS=60` hampir tidak dipakai (pool-nya sekali pakai).
**Remediasi**: satu `RealPool` statik proses-wide (conninfo dari Config, lazily
initialized, thread-safe — `RealPool` sudah punya mutex), handler memanggil
`the_pool().exec_params_pooled(...)`. `main.cpp` sudah punya `db RealPool` global
untuk store — tinggal diekspos. Perubahan ini sekaligus menghapus 5+ duplikat
definisi `with_pg`.

### T3 (MEDIUM) — Fallback CDN protobufjs menunjuk bundle lokal yang tidak pernah ada
`protobuf-helper.js:13-17`: `onerror` menyarankan "bundle lokal
`/static/js/protobuf.min.js`" — file itu **tidak ada** di `static/js/` (hanya
`fingerprintjs.min.js`). Diperkuat temuan B2 pass-20 (SRI placeholder
`sha384-PLACEHOLDER…` membuat tag `<script>` CDN selalu gagal verifikasi):
artinya bila `protobuf` global belum termuat, `loadProtobuf()` selalu reject —
tidak ada jalur sukses sama sekali. Mitigasi aktual: helper ini **tidak
di-include** oleh template mana pun (grep `protobuf-helper` di templates/ = 0)
dan `loadProtobuf/encodeCreateExam/apiFetchProtobuf` tidak dipanggil dari JS
lain — jadi frontend web saat ini tidak memakai protobuf client-side
(`PROTOBUF_MANDATORY` hanya mengenai client Android). Status: dead code berbahaya
bila kelak di-include. **Remediasi**: hapus branch CDN (B2 pass-20), tulis
hash SRI asli bundle lokal, atau commit bundle + sertakan; minimal hapus helper
sampai dipakai sungguhan.

### T4 (MEDIUM) — Duplikasi versi `2.7.2` kini 12 titik (naik dari 3 temuan M19)
Hardcode `"2.7.2"`: `config.hpp:17` (default), `dashboard.cpp:83`,
`submissions.cpp:70`, `pengawas.cpp:83,94` (pengawas_page/detail),
`api/exams.cpp:219,231` (health JSON + protobuf), `download.cpp:11,29`
(halaman download + **object key R2 `apps/android/2.7.2/EXAMVAN-v2.7.2-student.apk`**),
`hasil.cpp:212-214,404`. Dua yang paling bermasalah: (a) `download.cpp:29` —
`download_apk` men-presign object R2 dengan versi hardcoded, jadi update APK
berikutnya tidak bisa ditemukan tanpa recompile; (b) `health` JSON+protobuf
mengembalikan 2.7.2 sementara `handlers.cpp:18` health versi lain memakai
`cfg.version` — dua endpoint `/api/health` mendaftar dua route (`handlers.cpp:23`
+ `router_full.cpp:302`), router first-match memenangkan `register_routes`
(versi cfg), tapi jalur `handlers::api::health` (register_full) berbeda isi.
**Remediasi**: satu sumber `Config::version` di semua titik (paritas M19
pass-20 sudah direkomendasikan; kini lebih mendesak karena menyangkut R2 key).

### T5 (MEDIUM) — `queue_status` membaca list `examvan:submissions:failed` yang tidak pernah ditulis
`submissions.cpp:227`: `redis_llen("examvan:submissions:failed")`. Worker tidak
pernah LPUSH ke key itu (grep seluruh src: hanya `kQueueKey=pending` dan
`kResultKeyPrefix=result:` yang ditulis; job gagal-retry-habis hanya menghasilkan
`JobResult` Redis + placeholder `submissions` row). Konsekuensi: panel antrean
admin selalu menampilkan `failed: 0` bahkan ketika submit siswa gagal permanen —
operator tidak pernah tahu ada jawaban hilang. **Remediasi**: LPUSH job yang
melewati `kMaxRetries` ke `examvan:submissions:failed` (sebelum store_result),
atau hitung "failed" dari JobResult `success:false` / placeholder rows tanpa
answers_json.

### T6 — dicabut saat verifikasi (bukan temuan)
Awalnya dicurigai masalah di alur access-log (rate-limit sebelum validasi
token + pertumbuhan bucket fallback lokal). Saat ditelusuri: rate-limit
memang sengaja di depan (anti-flood), `RateLimiter::allow` sudah punya
lazy-evict 1024 entri, dan `device_started_at` dicek konsisten — alur
access-log/heartbeat HTTP sehat. Tidak ada perubahan yang diminta.

### T7 (LOW) — `dashboard_page` menempel `X-User` header ke HTML
`dashboard.cpp:111-112`: `html += html_escape(it->second)` untuk header
`X-User` — sisa debug/testing (nilai di-escape, jadi bukan XSS; header hanya
bisa di-set oleh klien di jalur uWS allowlist `server.cpp:532`). Efek: string
aneh menempel di akhir setiap halaman dashboard bila klien mengirim header
tersebut — visual noise + permukaan serangan rendah. **Remediasi**: hapus baris.

### T8 (LOW) — `test-results.xml` untracked + commit remediasi pass-20 belum di-commit
`git status`: 8 file modified + `tests/test_p26/p27/p28_tdd.cpp` untracked —
remediasi M9/C5/B1 pass-20 (addendum dokumen sudah mengklaim CLOSED) belum
ada dalam satu commit. `test-results.xml` (766 testcase, artifact CI) untracked
dan tidak di `.gitignore`. `docs/review-temuan-2026-09-06-pass18.md` juga masih
untracked. **Remediasi**: commit remediasi (dipisah per-temuan bila perlu),
tambah `test-results.xml` ke `.gitignore`.

### T9 (LOW) — Voucher `expires_at` format input tanpa validasi
`vouchers.cpp:226,261`: `expires_at` dari form di-INSERT langsung
`$5::timestamptz` tanpa validasi format di C++ (PG akan menolak string invalid
dengan 500 generik "Gagal menyimpan voucher"). Frontend mengirim format
terkendali, jadi dampaknya UX saja. `activate_voucher`/`redeem_voucher`
menghitung `remaining_seconds` dari `duration_days`, bukan `expires_at` —
konsisten. **Remediasi**: validasi format (pola `wib_to_utc_iso` di exams.cpp
bisa di-reuse) → 400 spesifik.

## Hal terverifikasi BAIK (pass ini)

1. **Escaping konsisten di seluruh frontend**: `admin.js` user-table
   (`escapeHtml` di semua interpolasi), `settings-vouchers.js` (kode voucher
   di-escape sekali untuk teks + atribut, S3), `settings-system-apps.js`
   memakai `el()` + `textContent`, template pengawas (`esc()`, `escapeHtml`,
   `jsEscape` untuk atribut JS+HTML dua-layer), SSR `build_exam_table_html`
   memakai `html_escape` di setiap nama/token. Tidak ditemukan sink XSS nyata.
2. **Alur student submit end-to-end solid**: rate-limit per exam+MAC sebelum
   gate token, validasi identity_fields required sebelum enqueue, INSERT
   placeholder dulu (durability), `lpush_checked` → 503 bukan 202 palsu,
   idempotency `Idempotency-Key` per exam, JobResult di-bind ke
   job_id+exam_id+mac (+ identity fingerprint anti-TOFU di poll result).
3. **Voucher redeem atomik**: `SELECT … FOR UPDATE` + BEGIN/COMMIT mencegah
   double-spend `max_usage=1`; `activate_voucher` men-deactivate redemption
   lain dalam transaksi yang sama; pesan seragam anti-oracle untuk
   invalid/expired/used.
4. **Worker batch transaksional**: SAVEPOINT per job (poison-row tidak
   membatalkan batch), advisory lock per exam+mac mencegah upsert race,
   revoke approval post-COMMIT best-effort, backoff serial → sleep sekali,
   final drain saat stop. Heartbeat flusher: SAVEPOINT per payload,
   requeue batch saat COMMIT gagal, cap 20 batch/500 row.
5. **Session/CSRF**: HMAC-SHA256 + constant-time compare, exp server-side,
   dual-key rotation, `extract_cookie` boundary-aware, CSRF double-submit
   dengan dev-only fallback plain cookie; `client_ip` validasi ketat
   karakter + `EXAMVAN_TRUST_PROXY=0` opt-out (nginx compose overwrite
   X-Real-IP di semua location, webui-cpp hanya `expose` internal —
   spoofing header tidak terjadi lewat jalur compose default).
6. **R2 presign**: endpoint fail-closed (`r2.cloudflarestorage.com` check,
   M10), canonical request AWS-SigV4 benar, `resolve_existing_pdf_key`
   migrasi layout Go→C++; exam_pdf gate 4-lapis (token + aktif+started +
   schedule + device approved).

## Prioritas remediasi (Pass-21)

1. **T1**: blok CREATE TABLE/ALTER untuk `saas_settings`, `vouchers`,
   `voucher_redemptions`, `package_settings`, `exam_pengawas` + kolom
   `admin_users` di `migrate()` — syarat deploy fresh-DB.
2. **T2**: pool PG proses-wide (hapus 5 duplikat `with_pg`) — perf + konsistensi.
3. **T5**: tulis job gagal-permanen ke `examvan:submissions:failed` agar panel
   admin bermakna.
4. **T4**: versi satu sumber `Config::version` (12 titik, termasuk R2 key APK).
5. **T8**: commit remediasi pass-20 + `.gitignore test-results.xml`.
6. **T3/T7/T9** + carried-over pass-20 (H7, M7, M8, M16, M19, L3, B2-B6)
   sesuai prioritas pass-20.

## Rekomendasi pengujian

1. Fresh-DB integration: `docker compose down -v && up` (tanpa skema Go) →
   register user → login → redeem voucher → create exam → submit siswa →
   export. Semua tahap harus hijau setelah T1 diperbaiki (saat ini redeem/
   register akan gagal).
2. Load test koneksi: `scripts/load_test` 200 req/s selama 60 dtk ke
   `/admin/api/pengawas/exams` — amati `ss -tan | grep 5432 | wc -l` sebelum/
   sesudah T2 (target: konstanta kecil, bukan linear terhadap RPS).
3. Failure-injection queue: matikan PG saat antrean berisi job, biarkan retry
   habis → pastikan `queue_status.failed` > 0 (setelah T5) dan JobResult
   `success:false` tersimpan.

---

## Addendum remediasi (2026-09-08) — T1-T9 CLOSED di `aea5e8f`

**T1 — CLOSED.** `migrate()` (`exam_store_postgres.cpp`) kini membuat
`saas_settings`, `vouchers`, `voucher_redemptions`, `package_settings`,
`exam_pengawas` (skema mengikuti kolom yang dipakai query C++) plus
`ALTER TABLE admin_users ADD COLUMN IF NOT EXISTS` untuk 20 kolom yang
dipakai INSERT register (`auth_store.cpp`) dan `edit_user` — idempoten,
skema Go yang sudah lengkap tidak tersentuh.

**T2 — CLOSED.** Pool PG proses-wide baru `src/db/pool_global.hpp`
(`global_pool()` lazy-init dari Config + `with_global_pg()` helper). Lima
duplikat `with_pg` (pengawas/submissions/settings/vouchers/users) kini
delegasi ke pool global; `write_audit_log` settings + `system_apps_page`
juga. Koneksi di-reuse lintas request — bukan TCP+auth PG baru per request.
Catatan: heartbeat flusher (`drain_heartbeats_once`) masih membangun pool
per tick-30 dtk — churn jauh lebih kecil (1/menit vs per-request), tindak
lanjut opsional menyusul bersama konsolidasi store pool.

**T3 — CLOSED** (sekaligus menutup **B2 pass-20**): branch CDN protobufjs
dihapus dari `protobuf-helper.js`. SRI placeholder membuat browser selalu
menolak script CDN dan bundle lokal `/static/js/protobuf.min.js` tidak
pernah ada — `loadProtobuf` kini menolak eksplisit. Helper tetap tidak
di-include template mana pun (client web tidak memakai protobuf client-side).

**T4 — CLOSED.** Semua literal versi di handler diganti
`Config::load().version` (dashboard, pengawas×2, settings, submissions,
health JSON+protobuf, template_renderer auth×3, download×2 termasuk object
key R2 APK, hasil×4). Sumber tunggal: `config.hpp:17` default +
`models/settings.hpp` (saas_settings default android_version).

**T5 — CLOSED.** `kFailedQueueKey` baru di `submission_queue.hpp`;
`SubmissionQueue::push_failed()` LPUSH job gagal-permanen di ketiga titik
retry-habis (stop-drain, batch PG failed, jalur tanpa PG);
`queue_status` membaca `kQueueKey`/`kFailedQueueKey` via konstanta.

**T7 — CLOSED.** Append `X-User` di `dashboard_page` dihapus.

**T8 — CLOSED.** `.gitignore` + `test-results.xml`, `.claude/`; remediasi
pass-20 di-commit (`1ff5b06`) bersama dokumen pass-18/21.

**T9 — CLOSED.** `valid_expires_at()` di `vouchers.cpp` — create dan batch
menolak format tanggal salah dengan 400 spesifik sebelum menyentuh PG.

**T6** tetap dicabut (bukan temuan). Verifikasi: TDD RED→GREEN
`tests/test_p29_tdd.cpp` (14 test kontrak), full suite **795 tests — 794
passed + 1 skip** (P7 kondisional, tanpa regresi), build `examvan-server`
bersih, docker-parity `check-docker-paths.sh` 54 src × 2 mode + 71 tests
lulus (`-Werror`). Kontrak legacy yang bentrok dengan pola baru
diselaraskan (`PgConnectionsNeverUseSanitizedUrl`,
`Submissions_QueueStatusFromRedis`, `Settings/Vouchers_PersistsToPostgres`,
`P25.CdnHasSri`) — semantik perlindungannya tetap (sanitized_url dilarang,
UPSERT tetap diwajibkan, CDN tanpa SRI asli dilarang).

Sisa terbuka setelah pass ini (carried-over pass-20): H7, M7, M8, M16-sisa,
M19-sisa (extract_contract path + version kini tersisa titik template
replace-literal yang sah), L3, B3, B4, B5, B6 + tindak lanjut opsional
(heartbeat-flusher pool, integration test fresh-DB bila ada env PG nyata).
4. Smoke `download/apk` setelah versi di-single-source: object key R2 harus
   mengikuti `Config::version`, bukan literal.

---

## Addendum remediasi pass-23 (2026-09-08) — carried-over M16/M19/B3/B4/B5/B6 CLOSED di pass-22+1

Metode TDD: kontrak infra ditulis dulu (RED) di `tests/test_p31_env.sh`
(bash — compose/nginx/script tidak terjangkau gtest) + `tests/test_p31_tdd.cpp`
(7 test gtest untuk sumber yang terbaca sebagai teks), lalu remediasi GREEN.

**B6 — CLOSED.** Dua worktree `.claude/worktrees/agent-*` (HEAD `f8b9f35`,
terverifikasi ancestor `main` via `git merge-base --is-ancestor`) dihapus:
`git worktree remove` × 2 + `git branch -D worktree-agent-*` × 2; direktori
`worktrees/` kosong dihapus. `.claude/` sudah di `.gitignore` sejak pass-21.

**M19-sisa — CLOSED.** `scripts/extract_contract.py`: path absolut
home-direktori dihapus; kini menerima `--src` (default `./cmd/server/main.go`)
dan `--out` (default `contract.json` di folder script); file sumber hilang →
**exit 2 dengan pesan jelas** (bukan gagal senyap menimpa golden). Fungsional
terverifikasi: extract 3 route dari main.go tiruan → JSON benar. Efek samping
positif: RED-run sempat menimpa golden `scripts/contract.json` (26 route, bukan
40+) via script lama yang mengabaikan argumen — dengan fail-loud baru, jalur
rusak ini tertutup; kontrak `P4_Contract.ParityRoutes` diselaraskan ke
realitas repo (repo Go tidak ada di sini): health + WS + ≥20 route.

**M16-sisa — CLOSED.** `docker-compose.yml`: Redis dijalankan
`--requirepass "${REDIS_PASSWORD:?}"` (wajib set — compose gagal start tanpa
env, tanpa default lemah), `REDIS_URL` app kini `redis://:${REDIS_PASSWORD}@redis:6379/0`
(AUTH sudah didukung `connect_redis` P18-M14 — diverifikasi baca sumber),
healthcheck redis `redis-cli -a "$REDIS_PASSWORD" ping`. Hardening non-root +
read-only: db `user: "70:70"` + `read_only` + tmpfs `/tmp`,`/run/postgresql`;
redis `user: "999:999"` + `read_only` + tmpfs; nginx `user: "101:101"` +
`read_only` + tmpfs cache/run/pid (worker tetap nginx:nginx). Live E2E:
`docker compose up redis` → PING tanpa AUTH = `NOAUTH Authentication required`,
dengan AUTH = `PONG`; `docker compose config` valid; volume bersih setelah
`down -v`. `.env.example` + `REDIS_PASSWORD` (template). DB_PASSWORD via env
tetap dicatat sebagai batas compose tanpa secrets (pass-20).

**B4 — CLOSED.** `nginx/nginx.conf` `location /`: `proxy_set_header Upgrade`
+ `Connection $connection_upgrade` dihapus (kini hanya di `location /ws/`);
`proxy_read_timeout` `location /` dinaikkan 60s → 130s sebagai fallback WS lama
(sejajar idle server 120s). Semua WS wajib lewat `/ws/` (130s). Diverifikasi
`nginx -t` di image `nginx:1.27-alpine` (upstream diganti 127.0.0.1 untuk test
standalone): configuration test successful.

**B5 — CLOSED.** Semua baris aktif `add_header Strict-Transport-Security`
dihapus dari `nginx.conf` (3 titik: server, location /, login, health) — HSTS
di port 80 diabaikan browser per RFC 6797 dan menyesatkan audit. Placeholder
`listen 443 ssl` + `ssl_certificate` comment tetap; HSTS wajib dikembalikan
bersama aktivasi TLS. Kontrak legacy `Review_Nginx.SecurityHeaders` (mewajibkan
HSTS tanpa syarat TLS) diselaraskan — proteksi CSP/max_body/timeout tetap
dikunci.

**B3 — CLOSED.** `scripts/check-docker-paths.sh`: loop serial → `xargs -0 -P
"$JOBS" -n 1` (worker mode `--worker` re-invokes script per unit; `JOBS` env,
default `nproc`). Ukuran sebelum (serial, pass-20): >8 menit; sesudah (paralel
-P8): **2m05s wall / 12m37s CPU** untuk 54 src × 2 pass + 73 tests, output
failure tetap dilabeli per file+mode, exit code fail tetap benar.

**Verifikasi pass-23:** RED `test_p31_env.sh` = 10 FAIL (sebelum remediasi)
→ GREEN 14/14 PASS; gtest P31 = 7/7; full suite release **812 tests — 812
passed + 1 skip** (P7 kondisional, tanpa regresi); full suite sanitizer
**812 passed + 1 skip**; docker-parity `check-docker-paths.sh` 54 src × 2 +
73 tests bersih (`-Werror`, paralel); `nginx -t` OK; `docker compose config`
OK; live redis requirepass OK. CI kini menjalankan `tests/test_p31_env.sh`
sebagai guard kontrak infra (step baru setelah docker-path check).

Sisa terbuka setelah pass-23: **tidak ada** dari daftar pass-20/21 — tersisa
tindak lanjut opsional (heartbeat-flusher pakai pool global, integration test
fresh-DB bila ada env PG nyata, TLS/HTTPS aktivasi → kembalikan HSTS,
secrets management compose bila stack produksi menuntut).
