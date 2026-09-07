# Review Ulang — Pass-20 (2026-09-07, verifikasi lanjutan Pass-19)

Baseline: HEAD `63119f9` (4 commit setelah docs Pass-19: build/CI docker-path parity,
static_path_safe shared, lint test layer). Suite penuh **765 tests, 764 passed + 1 skip**
(`P7_Frontend.JsGuardCount`, skip kondisional — no `package.json`), tanpa regresi.
Metode: build + full test suite lokal, spot-check `check-docker-paths.sh` (g++ `-fsyntax-only`
paritas Docker: src server.cpp/pengawas/exams/submission_queue/main EXIT=0 di kedua mode
uWS ON/OFF), grep verifikasi per temuan Pass-19 + review ulang diff `41cca10..63119f9`.

## Ringkasan eksekutif

Tidak ada temuan baru dari diff sejak Pass-19: perubahan berupa pemindahan `static_path_safe`
ke atas file (deklarasi-order, isi identik), relokasi `persist_submission_pending` + `json_escape`
di exams.cpp (isi identik, kini dokumentasi jelas per fungsi), pemindahan `write_audit_log` ke
atas `set_approval` di pengawas.cpp, dan penataan `set_lpush_checked` di main.cpp (format saja).
Diff-nya benign — namun verifikasi menegaskan **6 temuan Pass-19 masih OPEN tanpa perubahan**,
plus beberapa halus yang lolos verifikasi Pass-19 (audit-log tanpa atribusi, SRI placeholder,
`get_auto_approve` fail-open, kontradiksi nginx WS `Upgrade` di `location /`).

## Status verifikasi (per-item Pass-19)

### Masih OPEN — prioritas remediasi

1. **C5 — POSIX worker + acceptor masih `detach()`** — **CLOSED lihat Addendum**. (ARSIP: `server.cpp:599,608` sebelum remediasi).

2. **H7 — heartbeat/exam_completed tolak semua non-privileged** (`hub.cpp:152,209`,
   `privileged = admin_id != 0`). Student direct heartbeat selalu drop; tidak ada keputusan
   paritas Go (relay via pengawas vs direct-student) dan tidak ada test WS end-to-end.
   Bila frontend student mengirim heartbeat langsung, presence board pengawas kosong.

3. **M7 — cek scope submission masih 100% di router** (`router_full.cpp:195-226`).
   Handler `submissions.cpp`/`pengawas.cpp` tetap memercayai scope router. Split PG vs
   memory store (`active_store()->get_by_id` vs PG lookup) berisiko 403 salah atau bypass
   bila dua store divergen. Pindahkan cek ke handler + test matrix owner/delegate/super.

4. **M8 — bulk-toggle/bulk-delete exams hanya scope created_by/delegated_to**
   (`exams.cpp:1180,1206`): `exam_pengawas` dan operator-same-instansi tidak dihitung
   (beda dengan single-assign path). `edit_user` kini di-sanitize `sanitize_roles_json`
   (`users.cpp:90-92,329,453`) — bagian mass-assignment sudah baik; sisa scope bulk saja.

5. **M9-sisa — fallback sukses no-op** di `submissions.cpp:254` (delete: bila `with_pg`
   return tanpa mengeksekusi — DB down — jatuh ke `200 success` karena `result` kosong;
   bandingkan `set_approval:398-404` yang sudah benar cek `DATABASE_URL` → 503), dan
   `get_auto_approve:408-426` (PG down → `enabled=false` default tanpa indikator error;
   pengawas melihat auto-approve mati padahal DB down). Set approval sudah benar (503).

6. **M16-sisa — docker-compose**: Redis TANPA `requirepass` (`docker-compose.yml:26`),
   layanan redis/db/nginx tanpa `read_only`/`user` (hanya webui-cpp yang hardened
   `:70-72`). `DB_PASSWORD` tetap lewat env (inspect-visible) — batas compose, tidak
   bisa dihilangkan tanpa secrets, catat saja.

7. **M19-sisa — contract tooling mati**: `extract_contract.py:6` hardcode path absolut
   `/home/vannyezha/...` (gagal di CI/mesin lain); `parity_harness.py:11` hanya 4 path;
   keduanya tidak dipanggil CI. Versi app tetap hardcode `2.7.2` tiga kali di
   `template_renderer.cpp:167-169` + default `config.hpp:17` (Go ref 2.7.4).

8. **L3 — `mask_token` masih sized-mask** (`settings.cpp:86-90`): `std::string(t.size()-4,'*')`
   bocor panjang secret. Test `test_p17_tdd.cpp:301` mengecek "fixed mask" tapi ada
   fixed-mask impl versi sebelumnya — verifikasi: implementasi sekarang TETAP sized.

### Temuan baru (halus, lolos verifikasi sebelumnya)

- **B1 (MEDIUM) — Audit-log tanpa atribusi user.** `write_audit_log` (`pengawas.cpp:353-364`)
  selalu INSERT `user_id=NULL, username=''` — dipanggil dengan `""` (`:393`). Kolom ada,
  tabel ada (`exam_store_postgres.cpp:100`), tapi log tak bisa menjawab "siapa yang
  approve". `X-Internal-Admin-Id`/role sudah tersedia via `r2` headers (`router_full.cpp:231-234`)
  — teruskan username/admin_id dari session ke `set_approval` → `write_audit_log`.
  Juga: audit hanya menutup `set_approval`; `set_auto_approve`, bulk ops, delete
  submission, system-app upload/delete belum ada jejak audit.

- **B2 (MEDIUM) — SRI placeholder tidak dipakai apa pun.** `protobuf-helper.js:14`
  `integrity = 'sha384-PLACEHOLDER-PIN-LOCAL-BUNDLE'` — hash salah berarti browser
  MENOLAK script CDN (fail-closed, aman), tapi berarti fallback CDN praktis tidak
  berfungsi; test `P25.CdnHasSri` hanya cek substring "integrity" — lulus meski
  placeholder. Karena `s.onerror` sudah punya pesan bundle lokal, sebaiknya bundle
  lokal di-load langsung (drop CDN branch) atau pin hash asli.

- **B3 (MEDIUM) — `check-docker-paths.sh` belum dipanggil CI secara paritas penuh.**
  CI memanggil step "Docker-path syntax check" (`ci.yml:32-33`) — bagus — tapi full run
  2 pass src + 1 pass tests butuh >8 menit (diukur: ~2.6s/file → ~103 file × 3 pass,
  serial). Tanpa paralelisasi (`xargs -P`) step ini akan jadi bottleneck dan rawan
  di-skip/di-timeout. Tambah `-P$(nproc)` atau jadikan job terpisah.

- **B4 (LOW) — nginx WS timeout kontradiksi sisa.** `location /ws/` benar 130s
  (`nginx.conf:80`), tapi `location /` tetap kirim header `Upgrade`/`Connection
  $connection_upgrade` untuk semua request (`:44-45`) dengan `read_timeout 60s` (`:51`)
  — WS yang handshake via `location /` (mis. path `/ws` tanpa trailing slash, atau
  endpoint WS lain) tetap terputus 60s. Kecualikan header upgrade dari `location /`
  atau pastikan semua WS path di bawah `/ws/`.

- **B5 (LOW) — HSTS di HTTP `listen 80`.** `nginx.conf:37` mengirim
  `Strict-Transport-Security` di port 80 tanpa TLS — header diabaikan browser per RFC
  6797 (hanya valid via HTTPS) jadi tidak berbahaya, tapi menyesatkan saat audit dan
  `ssl_certificate` masih comment. Aktifkan TLS atau hapus header sampai TLS aktif.

- **B6 (LOW) — `.claude/worktrees/agent-*` untracked.** Dua worktree git dengan branch
  `worktree-agent-*` (HEAD `f8b9f35`, sudah ter-merge ke main — diverifikasi
  `merge-base --is-ancestor`). Tidak terlacak `.gitignore`; bersihkan dengan
  `git worktree remove` + `git branch -D worktree-agent-*` atau ignore `.claude/`.

### Terverifikasi FIXED / baik (sejak Pass-18, konfirmasi Pass-20)

- CI kini: stub-header syntax check step, install `libpq-dev libhiredis-dev libcurl4-openssl-dev`,
  sanitizer build (`ENABLE_SANITIZERS=ON`) + sanitizer test run (`ci.yml:32,40,48`).
- `check-docker-paths.sh` dirancang baik: dual-pass uWS ON/OFF, `-Werror` paritas Docker,
  test-layer parity, fail jelas (spot-check 3 file × 2 mode: EXIT=0 semua).
- `static_path_safe` kini dipakai di kedua jalur (posix `:245` + uWS `:403`) — traversal
  guard konsisten; ci.yml juga kini meng-compile kedua mode via script tsb.
- Logout/Cookies: `__Host-` vs `csrf_token` dev-switch di login (`login.cpp:80,85`), logout
  mirror (`logout.cpp:34`); session `exp` di-verify server-side (`cookie.cpp:142-148`).
- `sanitize_roles_json` mencegah mass-assignment superadmin (`users.cpp:90-92`).
- `router_full.cpp:51-53`: system-app route dikecualikan dari body-limit 5MB (105MB) —
  P17-C3 tertutup di layer router.
- Compose `webui-cpp`: read_only + no-new-privileges + tmpfs + healthcheck + mem/cpus limit.
- Version pin nginx `1.27-alpine`, postgres 16, redis 7 — tidak ada tag `latest`.

## Prioritas remediasi (Pass-20)

1. C5 posix: vector thread joinable + join di `stop()` (hostname/test parity dengan uWS).
2. M9: `delete_submission` + `get_auto_approve` cek `DATABASE_URL` → 503/503-indikator,
   samakan pola dengan `set_approval:398-404`.
3. B1: audit atribusi (admin_id/username dari X-Internal headers) + audit untuk
   set_auto_approve/bulk/delete/system-app.
4. H7: putuskan paritas Go heartbeat (relay vs direct) + 1 test WS end-to-end.
5. M7/M8: pindahkan cek scope submission ke handler; bulk ops hormati `exam_pengawas`
   + operator-instansi.
6. M16: Redis `requirepass` + `read_only`/`user:` untuk db/redis/nginx services.
7. M19/L3/B2/B6: perbaiki `extract_contract.py` path, mask_token fixed-length,
   SRI bundle lokal, bersihkan worktrees; version `2.7.2` → satu sumber (`Config::version`).

## Rekomendasi pengujian

1. Posix shutdown: `kill -TERM` saat koneksi aktif + queue pending — pastikan join
   semua thread < 5s dan tidak ada job hilang (dual build uWS ON/OFF).
2. `delete_submission`/`get_auto_approve`/`set_approval` matriks: PG down, PG slow,
   memory-store (unit) — status code konsisten (503 vs 200 no-op).
3. WS end-to-end student vs pengawas heartbeat (uWS + nginx `location /ws/`).
4. Bulk toggle/delete matrix: owner, delegated, pengawas-assigned, operator same/diff
   instansi, superadmin — bandingkan hasil single-delete.
5. `check-docker-paths.sh` di CI dengan paralelisasi; ukur durasi target < 5 menit.

---

## Addendum remediasi (2026-09-07)

**M9 — CLOSED.** `delete_submission` (`submissions.cpp:254-262`), `get_auto_approve`
(`pengawas.cpp:424-433`), dan `set_auto_approve` (`pengawas.cpp:460-466`) kini fail-closed:
DB dikonfigurasi (DATABASE_URL/Config) tapi `with_pg` gagal mengeksekusi → **503
"Database tidak tersedia"**, mengikuti pola `set_approval`. Mode memory/dev tanpa
DATABASE_URL tetap 200 (kompatibilitas unit test). Verifikasi: TDD RED→GREEN via
`tests/test_p26_tdd.cpp` (5 test kontrak sumber: fail-closed 503 + jalur 200 memory-mode
+ penanda kontrak "M9"), full suite **770 tests — 769 passed + 1 skip** (P7 skip kondisional,
tanpa regresi), sanitizer build lulus (147 test terkait), Docker-parity `-fsyntax-only`
uWS ON/OFF bersih. Sisa M9 (fallback sukses no-op di `users.cpp`) belum diubah —
tindak lanjut terpisah.

**C5 — CLOSED.** Jalur posix kini paritas graceful shutdown uWS:
- Worker posix (4–16) + acceptor disimpan sebagai `posix_threads_`
  (`std::vector<std::thread>`, member `server.hpp` tanpa guard — aman ODR/layout
  antara target Docker HAS_UWEBSOCKETS dan CI/lokal tanpa uWS). Zero `.detach()`
  di `server.cpp`.
- Worker loop (`posix_worker_loop`, `server.cpp` setelah `handle_client`) memakai
  state antrean statik `g_posix_q/g_posix_qmu/g_posix_qcv`: saat shutdown worker
  DRAIN sisa koneksi yang sudah diterima acceptor sebelum keluar (paritas uWS
  drain) — bukan langsung break.
- `Server::stop()` jalur posix: tutup listen fd → `notify_all` CV → join semua
  thread → clear; jalur uWS tetap `g_app->close()` + join `g_uWS_thread`.
- **Bug terkait ditemukan & diperbaiki: `main.cpp` tidak pernah memanggil
  `srv.stop()`** — join uWS tak pernah tereksekusi. Kini `srv.stop()` dipanggil
  di jalur SIGTERM/SIGINT sebelum drain queue (`main.cpp:165-168`).
- Verifikasi: TDD RED→GREEN `tests/test_p27_tdd.cpp` (5 test: larangan detach,
  member joinable, stop notify+join, srv.stop() di main, drain loop); live E2E
  posix build: server up → SIGTERM → exited cleanly <1s, port released; full
  suite **775 tests — 774 passed + 1 skip** (tanpa regresi, termasuk
  `ServerLive.HealthAndRouting` yang kini benar-benar men-join 5+ thread);
  sanitizer lulus (join menutup kebocoran thread yang sebelumnya tak terdeteksi
  karena detach); Docker-parity syntax server.cpp+main.cpp uWS ON/OFF bersih.

**B1 — CLOSED.** Audit-log kini beratribusi dan mencakup aksi admin destruktif:
- `router_full.cpp`: session username diteruskan via header baru
  **`X-Internal-Admin-Username`** (paritas Id/Role/Instansi/Super).
- `pengawas.cpp`: helper `audit_identity_from(req)` membaca header internal
  (fallback `""` — aksi tetap tercatat); `write_audit_log` menerima
  `user_id` + `username` (INSERT memakai `NULLIF($2,'')::int`, bukan `NULL`
  hardcoded). Terpasang di `set_approval` dan `set_auto_approve`.
- `exams.cpp`: helper paritas (identity unguarded, writer `#ifdef HAS_LIBPQ`,
  best-effort pool per panggilan). Terpasang di `bulk_toggle_exams`
  (per-exam `bulk_toggle:<status>`) dan `bulk_delete_exams`
  (`bulk_delete`, detail jumlah R2 key).
- `settings.cpp`: system-app `system_app_upload` (name/platform/version/bytes)
  dan `system_app_delete` (id + file_path) — keduanya aksi superadmin yang
  sebelumnya tak berjejak.
- Verifikasi: TDD RED→GREEN `tests/test_p28_tdd.cpp` (6 test kontrak: header
  username di router, larangan INSERT NULL/'' hardcoded, cakupan audit di
  set_approval/set_auto_approve/bulk ops/system-app); full suite **781 tests —
  780 passed + 1 skip** (tanpa regresi); sanitizer lulus; Docker-parity syntax
  4 file uWS ON/OFF bersih. Catatan: helper identitas disalin 3x (static per
  TU, tanpa header baru) — konsolidasi opsional saat refactor berikutnya.
