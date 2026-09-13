# Review Temuan — Pass 26 (2026-09-12)

> **Lingkup pass-26**: review menyeluruh seluruh area **D–K** pada working tree HEAD `a5046f1` + remediasi pass-25 yang belum di-commit (18 file dimodifikasi + 4 file baru: dokumen pass-25, dokumen ini, `tests/test_p36_tdd.cpp`, `tests/test_p37_tdd.cpp`). Review-only — tidak ada source yang diubah; dokumen ini adalah satu-satunya artefak baru. Pass ini (1) melengkapi pass-25 yang memotong area H/J/K per instruksi pengguna, (2) memverifikasi remediasi pass-25 per-line lewat pin P37 (**13/13 hijau**), (3) menulis kontrak TDD red-phase P36 untuk gelombang remediasi berikutnya (**14 pin merah = backlog berikutnya**), dan (4) menjalankan **review paritas menyeluruh vs `../EXAMVAN/webui` (Go)** atas permintaan eksplisit pengguna — see §5.9; dan (5) menyapu **area yang belum terpindai** pass sebelumnya (`scripts/`, CI, runbook cutover, compose, load_test, golden fixtures) — see §5.10; dan (6) **sapuan paritas UI byte-level** (template, static, nginx penyaji UI) vs kondisi webui **saat ini** — see §5.11, yang mengoreksi vonis template §5.9 dan premis J14.

---

## 1. Ringkasan Eksekutif

**Totals pass-26 (final — termasuk sapuan UI §5.11)**: **0 HIGH baru**, **17 MEDIUM baru**, **34 LOW baru**, 1 re-report (G7), 1 diretrak (J15) + 1 premis diretrak (J14), 3 catatan dokumen. Paritas vs Go: **MIRIP — ±85–90% paritas fungsional** (§5.9). Sapuan penutup §5.10 (area belum terpindai) menambah 4 MEDIUM + 10 LOW di atas draf awal; sapuan UI §5.11 (byte-level) menambah 2 MEDIUM (I39, I41) + 1 LOW (I40) dan mengoreksi J14 + vonis template §5.9.

| Grup | ID baru pass-26 |
|---|---|
| MEDIUM (17) | F18, F19, E-v, G20, G21, H9, J2, J4, J6, I22, I24, I25, I26, I27, I28, **I39, I41** (§5.11) — J14 ditarik premisnya (→ I39 + I24, §5.11) |
| LOW (34) | D21, G22, I21, I23, J16, H8, H10, H11, H12, H13, J1, J3, J5, J7, J8, J9, J10, J11, J12, J13, K8, K9, K10, I29–I38, **I40** (§5.11) |
| Re-report | G7 (drain_heartbeat_batch BEGIN tak diperiksa — kini dikunci pin P36 merah) |
| Diretrak | J15 (klaim route /logout hilang — bukti §7); premis J14 "port tanpa CSP" (port justru menyetel CSP di `nginx.conf:42/:55/:78/:104/:121` — substansi → I39 + I24, §5.11) |
| Catatan | P37 = 13 pin (bukan 12 seperti tertulis di dokumen pass-25); mapping seri ID P36 (§7); rekonstruksi teks K/J dari transkrip |

**Pola lintas-area pass-26**:

1. **Kolam HIGH pass-25 habis ter-remediasi** — P37 13/13 hijau (F12, I11, D7-bulk, E-l, E-m, E-d, G18, G14, G15, G17, F14 + G5/G6/G8 via P36 hijau). Risiko kini bergeser ke **residual kelas yang sama**: release-discipline yang belum menyapu semua jalur (F18, E-v), fallback fail-open yang tersisa (G21), dan hardening koneksi setengah jalan (G20, G22).
2. **Temuan menumpuk di aset port-only** (file yang tidak ada padanannya di Go, atau hanya hasil render): template `.rendered.html` membawa string baked/stale (J12/J13/K8/K9), dead code frontend (H8, H13), dan **CSP yang justru memblokir JS inline miliknya sendiri** (I39 — koreksi premis J14, §5.11). Tidak ada yang HIGH, tapi menambah permukaan serangan kecil dan utang kebersihan.
3. **Gap paritas terbesar bukan di handler** — handler/job sudah MIRIP ~85%. Gap ada di **tepi sistem**: batas upload nginx (I22), route admin minor (I23, H8), header & timeout nginx (I24), CSS publik (J16), dan divergensi perilaku (F19, expiry/cleanup, FeatureLock).
4. **14 pin P36 merah = kontrak remediasi gelombang berikutnya** — bukan regresi; lihat §3–§4.
5. **Lapis gerbang/tooling cutover yang didokumentasikan tidak pernah ditegakkan** — alat "Parity 0 diff" ternyata dead code (I25) dan status-code-only (I26), CI menjalankannya hanya dengan `--help` (I29), runbook harian memuat langkah yang tak bisa dijalankan (I27), dan prosedur rollback akan gagal (I28). Jaring pengaman dokumentasi cutover ini tidak akan berfungsi saat paling dibutuhkan (§5.10).
6. **UI port tertinggal tepat satu kampanye review+fix webui** — `templates/`+`static/` port bukan symlink melainkan salinan riil yang dibekukan 2026-08-25 16:21; webui menyelesaikan kampanye review+fix UI sendiri 2026-09-12 (12 MEDIUM + 35 LOW, 13 commit, terdokumentasi di `review_ui_halaman_web_2026-09-12.md`) → port mewarisi semua bug pra-fix (sink admin.js M22/M23/M25, guard cek_hasil M13 = I41) dan kehilangan seluruh 13 perbaikan; ditambah CSP nginx port yang memblokir JS inline template-nya sendiri (I39). Suite 887 test tidak melihat lapisan ini karena semua test bypass nginx (§5.11).

**Prioritas remediasi yang disarankan** (urutan dampak):
1. **G21** — fallback in-process `try_acquire_job` menyebabkan **double-run job lintas replika** saat Redis down (expiry/approval/cleanup/retention dieksekusi ganda). Fail-closed, hapus fallback, luruskan komentar yang kontradiktif.
2. **G20** — `AUTH` kirim username literal `"default"` (redis_real.cpp:54) → WRONGPASS senyap untuk Redis managed ACL non-default → seluruh fitur Redis mati diam-diam.
3. **I22 + I24 + I39** — nginx port: upload dibatasi 5 MB (nginx.conf:34) padahal Go 100 MB (main nginx :45) → upload PDF/R2 gagal untuk file besar; divergensi WS timeout/gzip/cache/healthz/HTTPS-redirect; dan CSP `script-src 'self'` tanpa nonce **mematikan semua `<script>` inline template** (I39, §5.11). Satu perbaikan CSP nonce + allowlist `challenges.cloudflare.com` menutup I39 + I24 butir 2 + separuh J2 sekaligus.
4. **Refresh UI dari webui `90ca7de`** (§5.11) — satu aksi menutup temuan terbanyak: re-copy `templates/`+`static/` menutup **I41** (guard double-submit cek_hasil), sink admin.js **M22/M23/M25 ≈ H9** (§7.3), **J16** (CSS publik), M14/M18/M21 + residual L42–L76; serentak serap blok nginx Go (**I40** worker; gzip/cache/healthz = I24 butir 3). Hanya 4 file static port yang butuh merge 3-arah (§5.11.3).
5. **H9** — export XML tanpa entity-escape (admin.js:1380-1424) → XML invalid/injectable oleh nama ujian/pengguna berisi `& < >`. Tertutup oleh refresh admin.js (item 4) — kunci dengan pin verifikasi pasca-copy.
6. **J6 + (J14 → I39/I24)** — jalur fallback tanpa CSRF; CSP port **dirombak**, bukan ditambah: nonce untuk JS inline (I39) + allowlist `challenges.cloudflare.com` (I24 butir 2). Premis lama J14 "port tanpa CSP" sudah ditarik (§5.11).
7. **F18 + E-v** — sisa jalur early-return tanpa `real.release()` (webhook OTP ×4, multipart 400).
8. **F19** — `/api/webhook` aktif di port (:316) padahal **mati di Go** (webhook.go: definisi tanpa registrasi) — konfirmasi niat, jangan aktifkan kode yang tak pernah diuji end-to-end di Go.
9. **I25–I28 (lapis gerbang cutover)** — sebelum runbook cutover/rollback dipercaya: wire ulang `shadow_proxy.py`, bandingkan body di `parity_harness.py`, segarkan `MIGRASI_STATUS.md`, perbaiki rollback sed (upstream duplikat) + hadirkan service Go di compose atau tandai rollback tidak tersedia. **I28 paling mendesak secara operasional** — rollback yang gagal justru saat insiden.
10. **Backlog P36** (14 pin merah, §3) — temuan LOW Batch-3 yang kontraknya terkunci.

---

## 2. Baseline & Timeline

| Pass | Tanggal | Repo state | Tests | Dokumen |
|---|---|---|---|---|
| pass-24 review | 2026-09-09 | `390fd0b` | 817 (816 pass + 1 skip) | `review-temuan-2026-09-09-pass24.md` |
| remediasi pass-24 | 2026-09-09/10 | `a3d9acd` + `a5046f1` (G4 sslmode) | — | commit T1–T9 + lanjutan |
| pass-25 review | 2026-09-10 | `a5046f1` | 855 (847 pass / 7 skip / 1 fail `UpdateSettings` — closed §4 pass-25, artefak TDD mid-cycle) | `review-temuan-2026-09-10-pass25.md` |
| remediasi pass-25 | 2026-09-10/11 | **UNCOMMITTED**: 18 file dimodifikasi + 4 untracked (doc pass-25, doc pass-26, `test_p36_tdd.cpp`, `test_p37_tdd.cpp`) | 887 total (P36 +19, P37 +13) | — |
| **pass-26 review (ini)** | 2026-09-12/13 | HEAD `a5046f1` + tree kotor (remediasi pass-25) | **887 (866 pass / 14 fail red-phase / 7 skip)** | dokumen ini |

Catatan penting: **baseline pass-26 adalah working tree, bukan commit** — remediasi pass-25 belum di-commit, sehingga angka build/test dan semua bukti file:line mengacu pada tree saat review, bukan `a5046f1` murni.

---

## 3. Status Build & Test

Build: Release, GCC 13, `WITH_UWEBSOCKETS=OFF` (stub POSIX), `WITH_PROTOBUF=ON`, `ENABLE_SANITIZERS=OFF`, ccache aktif, nproc=8.

```
887 tests / 866 PASS / 14 FAIL / 7 SKIP — CTEST_RC=8 — 30.23s
```

### 3.1 Ke-14 FAIL — pin red-phase P36 (BACKLOG, by design — lihat §4)

| # | Pin | Kontrak yang belum dipenuhi |
|---|---|---|
| 857 | `P36.D7_AuditLogCoverage` | 5 handler mutasi exams.cpp (create/update/delete/save_questions/delegate) wajib `write_audit_log(` |
| 858 | `P36.D8_ZeroRowUpdateReturns404` | `set_approval`/`set_auto_approve` (pengawas.cpp) wajib cek `PQcmdTuples` — 0 baris → 404 |
| 859 | `P36.D9_PengawasStateLive` | `pengawas_state` wajib `SELECT count(...)` live, bukan stub |
| 860 | `P36.D10_CommentMatchesFailClosed` | komentar `exam_bulk_scope_ok` wajib bilang fail-closed (masih menyebut "fail-open") |
| 861 | `P36.D11_UserIdZeroSavedAsNull` | `user_id>0 ? std::to_string : ""` agar NULLIF menyimpan NULL — di exams.cpp **dan** pengawas.cpp |
| 862 | `P36.D12_DashboardIncludesExamPengawas` | predikat dashboard.cpp wajib mencakup `exam_pengawas` |
| 863 | `P36.D13_DashboardSubmissionsNotHardcoded` | submissions dashboard dari `pending_submissions` live, bukan `"submissions":0}` hardcoded |
| 864 | `P36.Eb_ExportCsvWiredOrRemoved` | `export_submissions_csv` dideklarasikan/terdefinisi tapi tak pernah di-wire — wire atau hapus |
| 868 | `P36.G7_HeartbeatBeginChecked` | `BEGIN` drain_heartbeat_batch wajib dicek; gagal → requeue (`return 0`) |
| 870 | `P36.G10_IntegrationTestHygiene` | test_pg_integration wajib RAII cleanup + restore saas_settings, tanpa FLUSHDB |
| 871 | `P36.F8_R2HeadHasConnectTimeout` | `R2Client::verify` wajib `CURLOPT_CONNECTTIMEOUT` (anti-hang 15 s di jalur upload) |
| 872 | `P36.F11_SubmitUsesRealIp` | key rate-limit submit wajib `X-Real-IP`, bukan XFF mentah yang bisa dipalsukan |
| 873 | `P36.I5_HealthReadyEndpoint` | route `/api/health/ready` + handler `health_ready` (probe PG/Redis → 503) belum ada |
| 874 | `P36.DeadCode_VersionGateWired` | `middleware::version_gate` ada + teruji tapi tak pernah dipakai router (gagal di `test_p36_tdd.cpp:340`) |

### 3.2 Pin P36 yang HIJAU (5 — kontrak remediasi pass-25 terpenuhi)

`#856 D6_WibToUtcIsoStrict` (parser WIB ketat: full-consumption + validasi hari/bulan/kabisat, utils.cpp remediated), `#865 Ed_SubmissionsEarlyReturnReleases`, `#866 G5_StopNoPerJobBackoff` (Worker::stop tanpa sleep 200 s), `#867 G6_RedisAuthUsernameAndTimeout` (format `AUTH %s %s` + redisConnectWithTimeout), `#869 G8_EnqueueJobThreadLocal`.

### 3.3 Pin P37 — verifikasi remediasi pass-25: **13/13 PASS** (#875–887)

F12_WebhookNoReleaseBeforeUpdate, F12_ExecParamsRejectsNullConn, I11_DockerignoreMustNotExcludeCopiedSources, D7_BulkScopeComparesOwnerInstansi, El_InstansiUpdateUsesResultSentinel, Em_DeleteVoucherFailClosed, F14_RequestApprovalFailClosedOnPgDown, Ed_SubmissionsEveryPathReleases, G18_SystemAppsPageEveryPathReleases, G14_PushFailedSpoolsLocallyWhenRedisDown, G15_RedisConnectTimeoutReconnectAuthUser, G15_TryAcquireJobDistinguishesErrorFromUnlocked, G17_HealthProbesDependencies. → **Seluruh remediasi pass-25 terverifikasi hijau.** ("Pin hijau ≠ bebas bug baru hasil refactor" — residual dicatat sebagai F18/G20/G21/G22/E-v.)

### 3.4 SKIP (7)

`#197 P7_Frontend.JsGuardCount` (skip permanen) + `#817–823` PgIntegration ×6 (butuh `DATABASE_URL`; higienitasnya = pin G10 di backlog).

---

## 4. Catatan Root-Cause — 14 FAIL bukan regresi

Ke-14 kegagalan di §3.1 adalah **pin red-phase TDD yang sengaja ditulis untuk gagal**: test membaca source sebagai teks dan mencari marker kontrak perbaikan (pola `read_src` + `between` + `count_of`); marker belum ada → ASSERT gagal. Ini kontrak remediasi Batch-3, persis seperti pola P30–P35 gelombang sebelumnya yang berakhir hijau di P37. **Yang akan salah jika dibiarkan**: item D8/D9/D11/D12/D13 (respon/menyajikan data keliru), G7 (batch heartbeat hilang saat BEGIN gagal), F8/F11 (hang + evasi rate-limit), I5 (live vs ready tak terbedakan), Eb + version_gate (dead code yang terus membusuk).

Root-cause lintas backlog: (a) **handler menulis tanpa membaca hasilnya** (0-baris, fail-open) — keluarga D; (b) **resource dibuka tanpa diikat ke semua jalur keluar** (BEGIN, koneksi, dead code) — keluarga G/F/E-b; (c) **key kepercayaan diambil dari input klien** (XFF) — keluarga F.

---

## 5. Temuan per Area (ID baru pass-26)

> Kecuali disebut lain, severity dinilai pass-26. Temuan H/J/K dan K8–K10 berlabel **[rekonstruksi]**: teks asli laporan agen hilang saat kompaksi konteks; bukti file:line tetap diverifikasi ulang, redaksi direkonstruksi dari transkrip.

### 5.1 D — Admin exams & helper

**D21 (LOW) — Komentar remediasi D10 terpisah dari kodenya.** `src/handlers/admin/exams.cpp:938` — komentar `exam_bulk_scope_ok` berdiri jauh dari blok kode yang dideskripsikan (`:865–885`). Terkait pin merah `P36.D10_CommentMatchesFailClosed` (#860): komentar masih menyebut "fail-open" padahal kode fail-closed. Perbaikan backlog P36 sekaligus pindahkan komentar menempel ke blok.

### 5.2 E — Admin non-exams

**E-v (MEDIUM) — `system_apps_page`: jalur multipart-400 tanpa release.** `src/handlers/admin/settings.cpp:397` — early-return 400 untuk multipart tidak memanggil `real.release()`, kontras dengan `:403–404` yang me-release, dan DELETE `:389–391` yang me-release. Kelas yang sama dengan G18 yang sudah di-remediasi — jalur ini terlewat sapuan. Dampak: koneksi sehat di-CLOSE deleter (PQfinish) alih-alih kembali ke pool → churn TCP+auth PG per request admin yang menyentuh jalur ini.

### 5.3 F — API siswa & webhook

**F18 (MEDIUM) — Webhook OTP: 4 jalur early-return tanpa release (residual F12).** `src/handlers/api/webhook.cpp` — window utama (`:218–222` remediasi F12: koneksi TIDAK di-release antara SELECT per-user dan UPDATE; satu release di `:222` untuk kedua exit; `fail_closed_db` `:129–138`) sudah benar, tetapi masih ada **4 jalur early-return OTP** yang keluar tanpa `real.release()`. Kelas E-d/G18: koneksi di-close bukan dikembalikan. P36 belum mem-pin jalur ini — tambahkan pin di gelombang berikutnya agar sapuan lengkap terkunci.

**F19 (MEDIUM) — `/api/webhook` diaktifkan di port, padahal mati di Go.** Port `src/http/router_full.cpp:316` mendaftar `POST /api/webhook`; di Go, `webhook.go` hanya berisi definisi handler **tanpa satu pun registrasi route** (main.go tidak mendaftarnya). Port mengaktifkan kode yang tidak pernah berjalan (dan terbukti bermasalah — F1/F12/F18) di jalur keamanan (aktivasi akun via OTP). Konfirmasi niat: jika webhook memang fitur, aktifkan/samakan di Go dan beri hardening penuh; jika tidak, hapus registrasi di port. Terkait §5.9 paritas ("webhook dead-code divergence").

### 5.4 G — Infrastruktur (DB pool, Redis, queue, health)

**G20 (MEDIUM) — `AUTH` kirim username literal `"default"`.** `src/redis/redis_real.cpp:54` — format sudah `AUTH %s %s` (pin G6/G15 hijau), tetapi argumen username diisi konstanta `"default"`. Untuk Redis managed (ACL user non-default, umum di Redis Cloud/ElastiCache) → `WRONGPASS` senyap → semua fitur Redis (queue, leader election, heartbeat) mati diam-diam. Perbaikan: parse username dari URL (`redis://user:pass@host` — bagian user sebelum `:` di authority) atau env `REDIS_USERNAME`.

**G21 (MEDIUM) — `try_acquire_job`: fallback in-process menyebabkan double-run lintas replika + komentar kontradiktif.** `src/redis/client.cpp:16–48` — cabang HAS_HIREDIS sudah benar membedakan error vs unlocked (`if(ctx->err) return false;` `:31`; probe `redis_exists` `:32`), **tetapi** `catch(...){}` (`:36`) dan jalur fall-through ctx-null mendarat di lock in-process `g_locks[k]=now+ttl; return true;` (`:38–47`). Saat Redis down dan ada >1 replika, **setiap replika meng-acquire "lock" in-process-nya sendiri** → job expiry/approval/cleanup/retention dieksekusi ganda (aproval ganda, retensi delete ganda). Komentar kontradiktif: `:18–19` (P18-H6: fallback "hanya bila Redis tak terjangkau") vs `:26–30` (P37-G15: "return false TANPA fallback in-process") — kode mengimplementasikan yang pertama, kontrak menuntut yang kedua. Perbaikan: hapus fall-through fallback multi-replika (fail-closed `return false`), sisakan fallback hanya untuk mode single-node eksplisit, luruskan komentar.

**G22 (LOW) — `health_json` trio residual G17.** `src/server/server.cpp` — probe kini nyata (pin G17 hijau), tetapi: (1) `:689` setiap probe membuka koneksi pool baru per panggilan (bukan thread-local) → health check jadi beban saat diprobing agresif; (2) `:699–702` hasil degraded di-`(void)`-kan — status degraded tidak pernah dilaporkan; (3) `:703` hanya string `"down"` — tanpa kode/penyebab. Saran: reuse koneksi thread-local, laporkan degraded dengan penyebab. Terkait backlog `P36.I5` (#873) — endpoint `/api/health/ready` belum ada.

**G7 (RE-REPORT, MEDIUM) — `BEGIN` drain_heartbeat_batch tak diperiksa.** `src/queue/submission_queue.cpp:560` — ditemukan pass-25, dikonfirmasi masih terbuka pass-26, kini **dikunci pin merah** `P36.G7_HeartbeatBeginChecked` (#868): BEGIN gagal wajib requeue + `return 0`, jangan lanjut INSERT di koneksi rusak.

### 5.5 I — Build, deploy, nginx

**I21 (LOW) — Komentar Dockerfile tak akurat pasca-I11.** `Dockerfile:12–14` — komentar menyiratkan konteks COPY masih bermasalah padahal I11 sudah di-remediasi (.dockerignore tidak lagi mengecualikan sumber yang di-COPY; pin P37 I11 hijau). Baris `COPY .stylelintrc.json` kini punya baris sendiri (`:15`). Rapikan komentar agar tidak menyesatkan maintainer berikutnya.

**I22 (MEDIUM) — Batas upload 5 MB vs Go 100 MB (gap paritas #1).** `nginx.conf:34` — `client_max_body_size 5m`, sedangkan nginx Go memakai `100M` (main nginx `:45`). Upload PDF/berkas jawaban besar ditolak 413 di port padahal diizinkan di Go. Samakan ke 100M (atau nilai eksplisit yang disepakati).

**I23 (LOW) — Route minor Go yang hilang di port (gap paritas #2).** Belum terdaftar di `router_full.cpp`: `GET /+/index.html` dan `GET /+/robots.txt` (main.go:464–466), admin legacy redirects, `/admin/dashboard/redirect` (:666), `/admin/api/exams/:exam_id/pdf`, `/admin/api/submissions/:id/export_detail` (:777). Root-cause pasangan H8 (toggle-public-results/toggle-show-answers :716–717). Lihat §5.9 untuk daftar lengkap + penilaian.

**I24 (MEDIUM) — Divergensi nginx: timeout WS, CSP Turnstile, gzip/cache/healthz/HTTPS.** `nginx.conf` port: (1) `proxy_read_timeout` WS 130 s vs Go 86400 s — sesi proktor panjang terputus (kompromi sadar P23-B4 — tinjau ulang saat beban nyata); (2) CSP port tidak mengizinkan `challenges.cloudflare.com` (di-set di **5 lokasi**: `nginx.conf:42/:55/:78/:104/:121`) → **Turnstile mati saat toggle aktif, dan ini bukan hipotesis** — template port memang meng-wire widget: `reset_password.html:90-92` (`{{if .turnstile_enabled}}` + `<script src="https://challenges.cloudflare.com/turnstile/v0/api.js">`), `:173-176` (`div.cf-turnstile` + sitekey + `theme="dark"`), `:192` (`window.__turnstileEnabled`), `:320-334` (gate JS submit), `forgot_password.html:23` — script itu diblokir `script-src 'self'` → widget tak pernah render → form fail-closed menolak semua submit (terkait J2; basis kebijakan untuk disalin: nginx webui `:116`); (3) port tidak punya gzip, static-cache 365d, `/healthz`, HTTPS-redirect+HSTS yang dimiliki Go (HSTS di :80 memang sengaja dihilangkan — P23-B5). Bawa dari konfigurasi Go.

### 5.6 H — Frontend admin JS

**H8 (LOW) — Dead toggle calls di admin.js.** `admin.js:2881–2944` — panggilan toggle memanggil route yang tidak ada (root cause: route Go `toggle-public-results`/`toggle-show-answers` main.go:716–717 tidak dipordi; lihat I23). Tombol tampak hidup di UI tapi selalu gagal senyap.

**H9 (MEDIUM) — Export XML tanpa entity-escape.** `admin.js:1380–1424` — nilai (nama ujian/pengguna/instansi) disisipkan ke XML tanpa meng-escape `& < > ' "`. Nama yang mengandung karakter tersebut menghasilkan XML invalid (import gagal di sisi penerima) atau memungkinkan XML injection. Pakai entity-escape konsisten sebelum sisip.

**H10 (LOW) — Skip permanen tanpa tiket.** `tests/test_p7_tdd.cpp:24–28` — `P7_Frontend.JsGuardCount` di-skip permanen sejak lama tanpa catatan kapan boleh dihidupkan. Catat syarat unskip atau hapus.

**H11 (LOW) — settings-system-apps.js tanpa try/catch.** `settings-system-apps.js:428–433` — operasi async tanpa penanganan error; kegagalan meninggalkan UI state setengah jalan. Bungkus dengan feedback error.

**H12 (LOW) — Toggle tanpa revert UI saat gagal.** `admin.js:378–397` — state UI di-flip optimistik dan tidak dikembalikan saat respons gagal → UI berbohong tentang state server. Tambahkan revert di jalur error.

**H13 (LOW) — protobuf-helper.js port-only dead code.** File ada di port tanpa padanan Go aktif dan tanpa pemanggil — kandidat hapus, atau wire jika memang diniatkan (lihat pola version_gate, #874).

### 5.7 J — Template & rendered pair

**J1 (LOW) — pengawas_detail kehilangan pasangan rendered.** `pengawas_detail.html` `:91`/`:1094`/`:2018–2020` — variasi template tanpa padanan `.rendered.html`; pasangan rendered tidak lengkap (gap paritas template, §5.9).

**J2 (MEDIUM) — Divergensi Turnstile handler vs template (merged K7).** `login.cpp:149–155` menegakkan Turnstile, tetapi `login.rendered.html:137` merender widget turnstile tanpa sinkron konfigurasi (dan CSP port tidak mengizinkan `challenges.cloudflare.com` — I24). Jika Turnstile aktif di satu sisi dan tidak di sisi lain, verifikasi bisa dilewati atau selalu gagal. Samakan: widget muncul iff konfigurasi aktif, verifikasi handler konsisten.

**J3 (LOW) — `String.replace` first-only.** `:13–14` — pemakaian `replace` non-global untuk substitusi yang bisa muncul lebih dari sekali → hanya kemunculan pertama diganti. Ganti `replaceAll`/regex `g`.

**J4 (MEDIUM) — `answers_json` mentah di template.** `submissions.cpp:239` — jawaban disisipkan sebagai JSON mentah ke HTML admin. Jika konten jawaban membawa `</script>`/HTML, terjadi XSS di dashboard admin (akun admin berharga tinggi). Escape saat embed (atau embed via field bertipe data, bukan inline script).

**J5 (LOW) — Stub `pengawas_state`.** `:508–511` — state hardcoded, bukan dari DB. **≡ pin merah P36.D9_PengawasStateLive (#859)** — satu temuan, dua seri (catatan mapping §7).

**J6 (MEDIUM) — Jalur fallback tanpa CSRF.** Fallback render (non-rendered / jalur legacy) tidak menyuntik CSRF token → submit dari halaman fallback ditolak atau, lebih buruk, jalur yang mengabaikan CSRF terbuka. Pastikan semua jalur render memasang meta token yang dibaca `admin-core.js` apiFetch (`:78–83`).

**J7 (LOW) — robots.txt tidak tersaji.** Route `GET /+/robots.txt` tidak ada (I23) padahal Go menyajikannya (main.go:464–466) — kelayakan SEO/crawler hilang senyap.

**J8 (LOW) — `next` mati di satu jalur.** Parameter redirect `next` ada di template tetapi tidak dipakai/di-forward di satu jalur — konsisten-kan agar login mengembalikan pengguna ke halaman asal (bandingkan verifikasi ketat di login.cpp yang sudah bagus).

**J9 (LOW) — String disk stale baked `:2269`.** Bagian dari pola K9/K8 (§5.8): `.rendered.html` membawa snapshot string yang tidak lagi sinkron dengan runtime.

**J10 (LOW) — Form `action` hardcoded/tidak sinkron.** Beberapa form memakai action yang tidak cocok dengan route aktual → submit 404. Audit form vs `router_full.cpp`.

**J11 (LOW) — SVG duplikat.** Aset SVG sama didefinisikan berkali-kali dalam satu file → bloat & drift. Dedup.

**J12 (LOW) — `superadmin` baked ke template.** Nilai/label superadmin di-bake ke rendered HTML — kebocoran informasi kelas akun ke siapa pun yang bisa melihat sumber halaman, dan drift saat logika berubah.

**J13 (LOW) — `ADMIN_ID` baked.** ID admin di-bake ke rendered HTML — sama seperti J12; id seharusnya datang dari sesi/API, bukan snapshot.

**J14 (MEDIUM — premis DIRETRAK, substansi dilanjutkan ke I39 + I24; koreksi §5.11).** Klaim asli "tidak ada header Content-Security-Policy di port (nginx port tidak menyetelnya)" **terbukti salah**: port menyetel CSP di **5 lokasi** (`nginx.conf:42/:55/:78/:104/:121`). Masalah sebenarnya kebalikan arah dari klaim lama: (a) CSP terlalu **ketat** — `script-src 'self'` tanpa `'unsafe-inline'`/nonce/hash memblokir semua `<script>` inline milik template port sendiri → **I39** (§5.11); (b) CSP kehilangan origin Turnstile/font → **I24 butir 2**. Perbaikannya tetap satu aksi: rombak CSP (nonce + allowlist), bukan "tambahkan CSP yang tidak ada".

**J15 — DIRETRAK.** Klaim awal "route `/logout` tidak terdaftar di port" tidak benar. Bukti: port mendaftar `GET`+`POST /logout` (`router_full.cpp:263–264`) dan handler me-clear cookie (`logout.cpp:61–62`); Go juga mendaftarkan `GET /logout` → redirect `/login` (main.go:474). Tidak ada temuan yang selamat dari varian ini.

**Verified-good J-agent (tidak ada temuan)**: `esc()` via `createTextNode` (`pengawas_detail.html:1705–1710`); `formatApprovalStudentLabel` meng-escape name+mac (`:1413–1416`) sebelum sink `innerHTML` (`:1924`/`:1931`); CSRF double-submit solid di jalur utama; `build_exam_table_html` escaping konsisten; `json_escape_ci`; hardening login (rate-limit, lockout 5-gagal/15-menit, dummy-hash timing, validasi `next` ketat); cross-check route→template lengkap — tidak ada route admin yatim (users/vouchers/system-apps adalah tab hash settings via `/admin/api/*`); static mount benar; fingerprintjs di-serve lokal; semua field password `type=password`.

### 5.8 K — Rendered hygiene (rekonstruksi dari transkrip)

**K8 (LOW) — Konstanta `CSRF_TOKEN` baked mati.** `settings.rendered.html:2270` — token hex era-Go di-bake ke sumber halaman; hanya 2 hit grep (`settings.rendered.html:2270`, `settings.html:2255`); **tidak ada modul JS yang memakainya** — `admin-core.js` apiFetch (`:78–83`) selalu mengambil token fresh dari meta. Dampak: token basi terekspos di sumber halaman (usability nol, kebingungan auditor, permukaan kebocoran kecil). Hapus konstanta baked.

**K9 (LOW) — String disk stale baked.** `dashboard.rendered.html:1827` — snapshot string (konteks "badge") di-bake dan tidak lagi sinkron dengan runtime. *Teks lengkap laporan asli terpotong saat kompaksi; inti temuan dan lokasi terverifikasi.* Bersama J9/K8 ini satu pola: **hasil pre-render menumpuk snapshot yang membusuk** — butuh satu sapuan "de-bake" + aturan build.

**K10 (LOW) — Komentar CSS ter-mangle.** `pengawas.rendered.html:312–315` — komentar CSS rusak sisa minifikasi/pre-render, tidak berfungsi. Rapikan saat de-bake K9.

### 5.9 Paritas vs `../EXAMVAN/webui` (Go) — "apakah sudah mirip?"

**Vonis: MIRIP — ±85–90% paritas fungsional.** Handler inti dan job worker sudah padu; template padu di **nama** (19/19) tetapi **tidak di byte (3/19 — revisi §5.11)**; gap tersisa di tepi sistem. Catatan metode: `cmd/` dan `internal/` pada repo ini adalah symlink ke `../EXAMVAN/webui` — file Go selalu dibaca langsung dari sana.

| Dimensi | Vonis | Bukti kunci |
|---|---|---|
| Template | **MIRIP nama 19/19, byte 3/19** (revisi §5.11) | byte-identik hanya `login` (2026-08-24 16:58), `head` (08-24 13:26), `settings-tabs` (08-17 19:20) — persis file yang **tidak** disentuh kampanye UI webui 12-Sep; 16/19 berbeda — seluruhnya evolusi webui pasca-salina (mtime port seragam 2026-08-25 16:21 vs webui 12-Sep 15:24–18:41); +13 `.rendered.html` port-only; gap J1; klasifikasi per-file §5.11.2 |
| Static (JS/CSS) | **MIRIP-dengan-gap ~85%** | runtime JS 13/13; CSS 6/8 — hilang `public-auth.css` (2.707 B) + `public-toast.css` (5.891 B) → **J16**; `theme.css` −2.9 KB; test `.mjs` 77 vs ~90 (batch uiux-21–39); protobuf-helper.js dead H13 |
| Route | **PARSIAL ~85%** | Go main.go 26 top-level vs port 103 `r.add`; hilang: `GET /+/index.html`, `GET /+/robots.txt` (main.go:464–466), admin legacy redirects, `/admin/dashboard/redirect` (:666), toggle-public-results/show-answers (:716–717; =H8), `/admin/api/exams/:exam_id/pdf`, `/admin/api/submissions/:id/export_detail` (:777), middleware FeatureLockRequired (:663/:691/:1034–1035), `timeout.go` → I23; ekstra port: `POST /api/webhook` (:316) mengaktifkan dead code Go → F19; `/ws/:room_id` beda disengaja (M7) |
| Handler/job | **MIRIP ~85%** | 3/3 job 1:1 (expiry/cleanup/retention), leader election dua-duanya; HeartbeatFlusher ada (main.cpp:164–165, start jika redis_ok && db_up); **divergensi**: expiry 30 s (expiry_job.go:16) vs 3600 s (main.cpp:167); cleanup/retention dinamis dari DB (Go) vs hardcoded 1800/86400 s (main.cpp:168–169); webhook dead-code (F19) |
| Misc/nginx/config | **PARSIAL ~65%** | nginx: upload 5m vs 100M (I22); WS `proxy_read_timeout` 130 s vs 86400 s; CSP `script-src 'self'` di 5 lokasi — memblokir JS inline template (I39) + tanpa origin Turnstile (I24; **koreksi J14**: port PUNYA CSP di `nginx.conf:42/:55/:78/:104/:121`); kapasitas worker 4096 vs 10240 + `rlimit 65535` (I40); port tanpa gzip/static-cache-365d/`/healthz`/HTTPS-redirect+HSTS (I24); `seed.sql` tidak ada di root port; login lockout in-memory (login.cpp:26/:29/:181–192/:239–247) vs Go persistent; port-only: cutover map `$backend`, rate-limit POST-only login (P33-I2), carve-out system-apps 105 m (P33-I1) |

**Top-5 gap paritas** (urutan dampak): (1) batas upload 5 MB vs 100 MB (I22); (2) route admin minor hilang (I23/H8 — toggle, PDF, export_detail, dashboard/redirect, robots.txt); (3) divergensi nginx (I24+I39+I40 — WS 130 s, CSP memblokir JS inline + Turnstile, tanpa gzip/cache/healthz/HTTPS-redirect, worker 4096); (4) CSS publik hilang 8.6 KB (J16); (5) divergensi perilaku (F19 webhook, expiry 30 s→3600 s, cleanup hardcoded, tanpa FeatureLock/timeout middleware, lockout in-memory). Tambahan §5.11: (6) **UI tertinggal tepat satu kampanye webui** — 16/19 template + 14 static produksi berbeda; port mewarisi sink pra-fix M22/M23/M25 dan kehilangan seluruh 13 perbaikan.

**Positif paritas**: login-lockout ada di keduanya; `GET /api/time` ada di keduanya (router_full.cpp:292); `GET`+`POST /logout` ada di keduanya (:263–264); CSRF double-submit solid di jalur utama; escaping frontend inti solid (daftar verified-good §5.7).

**J16 (LOW) — CSS publik hilang.** `public-auth.css` (2.707 B) dan `public-toast.css` (5.891 B) ada di Go static, tidak dipordikan → halaman publik kehilangan styling autentikasi/toast. Copy + mount.

### 5.10 Area belum terpindai (scripts/tooling, CI, runbook cutover, compose, load_test)

Sapuan penutup atas permintaan pengguna ("lanjutkan ke bagian yang belum terpindai"): lapis tooling/runbook yang tidak tersentuh pass sebelumnya — skrip root `scripts/` (selain stubs/check-docker-paths yang sudah terverifikasi), `.github/workflows/ci.yml`, `docs-cutover.md`, `MIGRASI_STATUS.md`, `docker-compose.yml` (sebagai konteks runbook), golden fixtures, dan `scripts/load_test/`. Hasil: **4 MEDIUM + 10 LOW** — dengan satu pola lintas-file: **lapis gerbang cutover yang didokumentasikan ternyata tidak pernah ditegakkan** (poin 5 §1).

**I25 (MEDIUM) — `shadow_proxy.py` dead code, dikutip sebagai alat gerbang cutover.** `scripts/shadow_proxy.py:9` mendefinisikan `fetch()`, `:19` `compare()`, tetapi `__main__` (`:27`) hanya mencetak banner — keduanya tidak pernah dipanggil. Padahal `docs-cutover.md:18` menjadikannya kriteria "Parity 0 diff (scripts/parity_harness.py + shadow_proxy.py)", `:33` menjadikannya langkah verifikasi, dan `MIGRASI_STATUS.md:32` langkah harian. Wire loop proxy-nya atau hapus dari runbook — nama alat di gerbang kritis tidak boleh menghias dokumen tanpa berjalan.

**I26 (MEDIUM) — `parity_harness.py` membandingkan status code saja.** `scripts/parity_harness.py:19–22`: `gs,gb=fetch(a.go,p)` / `cs,cb=fetch(a.cpp,p)` lalu `ok = gs==cs` — body (`gb`/`cb`) diambil tetapi tidak pernah dibandingkan; respons 200 dengan body salah dinyatakan lolos. Dokstring `:3` ("compare Go vs C++ responses", dok 04 §2) hanya setengah benar. Gabungan I25+I26: kriteria "Parity 0 diff" jauh lebih lemah dari klaimnya.

**I27 (MEDIUM) — `MIGRASI_STATUS.md` runbook basi.** `:28` "`# 105 PASSED`" (realita 887 test / 866 pass); `:32` langkah harian menjalankan `scripts/shadow_parity.py` di dalam kontainer Go yang dihapus 2026-08-26 (`:3` mencatat penghapusannya sendiri); `:39` alamat rollback `127.0.0.1:5001` vs nama service `examvan-go-server:5000` tidak konsisten; `:17` PID soak 16751 hardcoded. Runbook yang tidak bisa dijalankan baris-per-baris bukan runbook.

**I28 (MEDIUM, paling mendesak secara operasional) — rollback `sed` menciptakan upstream duplikat + service target tidak ada.** `docs-cutover.md:25` menginstruksikan `sed -i 's/cpp_backend/go_backend/' nginx/nginx.conf` — menulis ulang `nginx/nginx.conf:8` (`upstream cpp_backend { server webui-cpp:5000; }`) menjadi `upstream go_backend` **kedua** (yang pertama sudah ada di `:7`, `server 127.0.0.1:5001 down`) → nginx `[emerg] duplicate upstream` → `docker exec nginx nginx -s reload` gagal total. Dan `docker-compose.yml` tidak memuat service Go sama sekali (hanya db, redis, webui-cpp, nginx) → target rollback tidak ada. Saat insiden nyata, prosedur rollback terdokumentasi gagal justru di momen paling dibutuhkan. Perbaikan: satu `upstream go_backend` + `sed` menukar baris dispatch/proxy_pass (bukan nama upstream), dan putuskan ketersediaan service Go (compose terpisah / image ter-tag) sebelum rollback dijanjikan di dokumen.

**I29 (LOW) — CI parity check hanya `--help`.** `.github/workflows/ci.yml:60` menjalankan `parity_harness.py --help && shadow_proxy.py --help` — memvalidasi argparse, bukan perbandingan nyata.

**I30 (LOW) — nama step CI basi.** `ci.yml:46` "Unit tests (248)" vs realita 887 test.

**I31 (LOW) — `shadow_parity.py` dokstring melebihkan.** `:5` klaim "key & tipe sama", tetapi `keys_of()` (`:28`) = `set(json.loads(body).keys())` → nama key top-level saja, tanpa tipe/nested; kelas "redirect" yang didokumentasikan (`:7`) tidak pernah dieksersisi `CASES`.

**I32 (LOW) — `.stylelintrc.json` guard dorman.** Tidak ada `package.json` di repo dan stylelint tidak pernah masuk CI — temuan seri J (CSS) hidup tanpa guard berjalan. Didokumentasikan sebagai "fase 2" (jujur), tetapi sampai dijalankan, guard tidak mencegah regresi CSS baru.

**I33 (LOW) — ekspektasi health runbook tidak cocok.** `docs-cutover.md:31` meminta `curl /api/health` → `{"status":"ok"}`, sedangkan golden `scripts/fixtures/golden/_api_health.json:1` menegaskan `"status":"healthy"`. Operator yang mengikuti runbook harfiah akan menandai server sehat sebagai "salah".

**I34 (LOW) — `f8_dashboard.sh` PID hardcoded.** `:9` `ps -p 16751 -o etime=` rapuh setelah restart proses; sampel `-e TARGET_VUS=200 -e HOLD_MS=5000` tidak cocok dengan default `k6_ws_steady.js` (3000/60000).

**I35 (LOW) — golden fixtures basi.** Versi `"2.7.3-983a6cca"` (sisi Go) tercantum di `_.json`, `_api_health.json`, `_download.json` (capture 2026-08-25) sementara port berada di 2.7.2. Interaksi: golden basi + komparator dangkal (I26/I31) = kepercayaan "0 diff" palsu.

**I36 (LOW) — `soak_check.sh` baseline RSS sampel pertama.** Sampel RSS pertama diambil pra-warmup → bisa memakan headroom deteksi; sisanya solid (resolusi PID per-iterasi, deteksi restart, threshold 120%, logika verdict).

**I37 (LOW) — `alur-public.md` catatan capture legacy salah.** `register_confirm.rendered.html` ternyata halaman login admin; `reset-password.rendered.html` halaman forgot. Catatan legacy + workaround (nama underscore + fallback `.html`) sudah ada di tempat — pastikan catatannya terbaca sebagai peringatan, bukan kebingungan baru.

**I38 (LOW) — `extract_contract.py` auth selalu "unknown".** `:21` menulis `"auth": "unknown"` untuk semua route; regex hanya menangkap bentuk `r.Method("path")` langsung — tanpa grup/rantai middleware.

**Bersih (dipindai, tanpa temuan)**: `scripts/stubs/` (4 header, jujur "syntax-check only"), `.clang-format` (Google, C++20, kolom 100), `scripts/check-docker-paths.sh` (remediasi paralel P23-B3 terverifikasi per-baris: `set -uo pipefail`, `BASE_FLAGS -isystem` stubs, dual-pass, `xargs -0 -P`), `scripts/contract.json` (artefak 3015 B), `scripts/load_test/` (3 skrip k6: `k6_ws_docker.sh`, `k6_ws.js`, `k6_ws_steady.js`).

### 5.11 Paritas UI byte-level vs webui saat ini (template, static, nginx)

Sapuan atas permintaan eksplisit pengguna ("dari segi UI, review dan bandingkan dengan webui yang sekarang") atas seluruh lapis UI: 19 template live + 13 `.rendered.html` port-only, `static/` (produksi + test), dan `nginx/nginx.conf` sebagai penyajinya. **Vonis: template hanya 3/19 byte-identik — jauh dari "~100%" draf awal §5.9 — tetapi root-cause-nya tunggal dan terdokumentasi penuh: webui menjalankan kampanye review+fix UI sendiri pada 2026-09-12, SETELAH salinan port dibekukan.** `cmd/`+`internal/` port adalah symlink ke webui (selalu segar); `templates/`+`static/` adalah **salinan riil** yang berhenti bergerak 2026-08-25 16:21.

#### 5.11.1 Bukti mtime — drift 100% berasal dari sisi webui

- Port: ke-19 template live seragam mtime **2026-08-25 16:21** — tidak ada satu pun template port yang disentuh sejak disalin.
- Webui: 3 file pra-kampanye — `settings-tabs` (2026-08-17 19:20), `head` (2026-08-24 13:26), `login` (2026-08-24 16:58); 16 file kampanye 12-Sep 15:24–18:41: `cek_hasil`+`forgot_password` 15:24, `shared` 15:45, `nav` 16:28, `hasil` 17:06, `download` 17:07, `register`+`register_confirm` 17:13, `reset_password` 17:14, `index` 17:49, `settings` 18:14, `dashboard`+`pengawas`+`submissions` 18:40, `svg-symbols`+`pengawas_detail` 18:41.
- `diff -rq -x '*.rendered.html'` kedua pohon template: set file yang berbeda = **persis 16 file kampanye**; 3 sisanya byte-identik. Korelasi 16/16 → drift bukan edit sisi port; seluruhnya evolusi webui.
- Kampanye webui terdokumentasi: `../EXAMVAN/review_ui_halaman_web_2026-09-12.md` (652 baris) — **12 MEDIUM (M16–M27) + 35 LOW (L42–L76)**, baseline `main@3c34337`, **13 commit fix** hingga HEAD `90ca7de` (`422dc8b` cleanup, `521c196` −30 dead func, `4033735` token dashboard, `245b4f7` M15 @import, `06c3b43`+`f88ef27` L-tier, `428568c` sink admin.js M22/M23/M25, `594c675` M24, `71c19ec` M16, `ab64bd9` M17–M20, `e86ecda` M26+M27, `3305d0d` M21, `90ca7de` L-tier).
- **Konsekuensi**: port = snapshot pra-kampanye → **mewarisi semua bug pra-fix DAN kehilangan seluruh 13 perbaikan**.

#### 5.11.2 Klasifikasi diff per-template (16 file)

| Template | Isi diff utama | Fix webui yang hilang di port |
|---|---|---|
| `settings.html` (794 baris diff) | webui **menghapus** komponen `.tooltip` custom — 5 pemakaian dimigrasi ke atribut `title` native; ~60 baris CSS (webui `:269-274`) diganti komentar yang mengutip trade-off review-nya | M18/L28 — port masih membawa blok `.tooltip::after/:before/:hover` penuh |
| `cek_hasil.html` (40 baris) | toast skin/host + include `admin-core.js` + `role="alert"` element-level (L67) + **guard anti double-submit penuh** (webui `:69-98`: disable tombol + "Memeriksa…" + restore bfcache `pageshow`) | M13 + M21 + L67 → **port tanpa guard = I41** |
| `forgot_password.html` (11 baris) | toast skin/host + `admin-core.js` | M21 |
| `index.html` (16 baris) | Indonesianisasi copy: webui `:11` "Kiosk Mode terkelola (Device Owner)", `:49` "Rendah, Sedang & Tinggi", `:90` "Skoring Parsial", `:114` "Mulai Tanpa Ribet" | M14 |
| `nav.html` | token-sweep: webui `:128–152` `rgb(var(--rgb-white))`/`rgba(var(--rgb-white),…)` vs port hardcoded `#fff`/`rgba(255,255,255,…)`; komentar webui `:5-7` mengutip dokumen review | kelas M27 (token) |
| 11 sisanya (`dashboard`, `pengawas`, `pengawas_detail`, `submissions`, `shared`, `hasil`, `download`, `register`, `register_confirm`, `reset_password`, `svg-symbols`) | kelas yang sama: unifikasi toast (M21), aksesibilitas aria (M17/M20/M24), token CSS, sprite lokal `hasil` (M26) | residual L42–L76 |

#### 5.11.3 Static

- **14 file produksi berbeda** — terbesar `js/admin.js` 214.408 B (port) vs 215.938 B (webui; +1.530 B = fix `428568c`); lainnya: `admin-core.js`, `theme.css` (8.527 vs 11.414 B), `settings-vouchers.js`, `admin-base.css`, `hasil.css`, `public-desktop.css`, `public-mobile.css`, `settings-system-apps.js`, `settings-users.js`, `settings-billing.js`, `settings-voucher-audit.js`, `settings-general.js`, `settings-packages.js`.
- **Sink pra-fix admin.js TERKONFIRMASI di port**: M22 `admin.js:1147` + `:3089` (checkbox `innerHTML` menyisip `p.id` tanpa escape), M23 `:629` (`target="_blank"` tanpa `rel="noopener"`), M25 `:1386` (`partial_scoring="${q.partial_scoring}"` mentah ke XML export — **bidang yang sama dengan H9**). Webui sudah memperbaiki ketiganya (`:1122`/`:3064`/`:604`/`:1360-1392`).
- **22 file webui-only**: `public-auth.css` (2.707 B) + `public-toast.css` (5.891 B) = **J16**, + 20 test `.mjs` (batch18–batch39). **1 file port-only**: `protobuf-helper.js` (H13, disengaja). **24 test `.mjs` shared berbeda**.
- Port menyunting **4 file static** pasca-salin (admin.js + `batch3-dashboard.test.mjs` 5 Sep; `settings-system-apps.js` 6 Sep; `protobuf-helper.js` 8 Sep) — edit port **tidak beririsan** dengan fix webui 12-Sep → keduanya harus di-merge 3-arah, bukan salah satu dipilih.

#### 5.11.4 Temuan baru formal

**I39 (MEDIUM) — CSP nginx port memblokir semua `<script>` inline template-nya sendiri.** Port menyetel CSP di **5 lokasi** (`nginx/nginx.conf:42/:55/:78/:104/:121`) dengan `script-src 'self'` — tanpa `'unsafe-inline'`/nonce/hash. Padahal hampir semua template port memakai inline JS: `hasil.html:298`, `cek_hasil.html`, `register.html:343`, `register_confirm.html:288`, `shared.html:40/:102/:989`, `forgot_password.html:75`, `reset_password.html:191`, `download.html`, `admin/dashboard.html`×2, `admin/settings.html`×2, `admin/submissions.html`, `admin/pengawas.html`, `admin/pengawas_detail.html`, `admin/login.html`, `admin/partials/nav.html`×2, `admin/partials/head.html`. Disajikan lewat nginx port, seluruh script itu diblokir browser → halaman kehilangan interaktivitas. Header CSP ganda di-merge **intersective** (kebijakan terketat menang) — header backend yang lebih longgar tidak menolong. Suite 887 test tidak melihat lapisan ini karena semua test menguji handler/render langsung (bypass nginx). **Koreksi J14**: klaim "port tanpa CSP" salah arah — port PUNYA CSP, justru terlalu ketat untuk kontennya sendiri. Perbaikan: nonce per-request (`script-src 'self' 'nonce-…'`) + allowlist Turnstile `challenges.cloudflare.com` (menutup I24 butir 2 + separuh J2) — basis kebijakan: nginx webui `:116`.

**I40 (LOW) — Kapasitas worker nginx port di bawah Go.** Port hanya `worker_processes auto; events { worker_connections 4096; }`. Go menambah `worker_rlimit_nofile 65535` + `worker_connections 10240` + `use epoll` + `multi_accept on`. Dengan beban WS proktor (koneksi panjang per siswa), port menyentuh batas koneksi jauh lebih cepat. Bawa blok dari Go.

**I41 (MEDIUM) — `cek_hasil` tanpa guard double-submit (fix webui M13 tidak terport).** Bukti di §5.11.2: webui menambah guard penuh (disable + "Memeriksa…" + restore bfcache); port tidak punya — klik ganda / resubmit form cek hasil tetap mungkin (spam request, proses duplikat). Contoh konkret pola umum §5.11: bug yang sudah diperbaiki webui tetap hidup di port.

#### 5.11.5 Diff nginx selain I39/I40/I24 (konteks)

Port-extra (BUKAN gap): `map $uri $backend` cutover dual-backend, rate-limit login 2 r/s + `limit_conn addr 10` (P33-I2), upstream `go_backend { … down }` untuk rollback — desain cutover yang tidak dimiliki Go. Webui-extra: alias internal `/internal/pdf/` (menegaskan gap PDF di I23). Divergensi yang tetap dilaporkan meski sadar: HSTS tidak dikirim di :80 (P23-B5), WS 130 s vs 86400 s (P23-B4) — keduanya di I24 agar keputusannya ditinjau ulang saat beban nyata.

**Rekomendasi (satu aksi, dampak terbesar)**: re-copy `templates/`+`static/` dari webui HEAD `90ca7de`, lalu merge 3-arah hanya untuk 4 file yang diedit port (admin.js, settings-system-apps.js — edit port 5/6-Sep vs fix webui 12-Sep — plus protobuf-helper.js port-only dan `batch3-dashboard.test.mjs`), serentak dengan perombakan CSP (I39) + serapan blok nginx Go (I40 + butir I24). Re-copy template aman total: port tidak punya satu pun edit template pasca-salin (bukti mtime §5.11.1).

---

## 6. Closed (diverifikasi pass-26 via P37 13/13 + P36 5 pin hijau)

| Temuan | Remediasi | Status |
|---|---|---|
| F12 webhook release-before-use + null-conn guard | `webhook.cpp` + `pool_real.cpp` (`if(!c)`) | **CLOSED** (parsial — residual F18: 4 jalur OTP tanpa release) |
| I11 .dockerignore vs COPY | `.dockerignore`/`Dockerfile` | **CLOSED** (catatan komentar: I21) |
| D7-bulk lintas instansi | `me.instansi=owner.instansi` di JOIN owner | **CLOSED** |
| E-l instansi_update 503-selamanya | sentinel `result=="ok"` | **CLOSED** |
| E-m delete_voucher fake-200 | gate 503 `pg_configured_from_env()` | **CLOSED** |
| F14 request_approval fake-200 | `pg_used` flag | **CLOSED** |
| E-d release-discipline submissions | pre-release scope-deny + final release | **CLOSED** (residual kelas: E-v, F18) |
| G18 system_apps_page release | 3× `real.release(` | **CLOSED** (residual: E-v) |
| G14 push_failed spool | `append_spool_line` + `kSpoolPath` JSONL | **CLOSED** |
| G15 hiredis hardening | `redisConnectWithTimeout` + `AUTH %s %s` + `redis_reset` + SELECT dicek + probe EXISTS | **CLOSED parsial** (residual G20 "default", G21 fallback) |
| G17 health probe | `global_pool` + `redis_ping` + `LLEN` | **CLOSED parsial** (residual G22) |
| G5 Worker::stop 200 s | tanpa `retry_backoff_ms` (pin P36 #866 hijau) | **CLOSED** |
| G6 AUTH format + timeout | pin P36 #867 hijau | **CLOSED parsial** (residual G20) |
| G8 enqueue thread-local | pin P36 #869 hijau | **CLOSED** |
| D6 parser WIB ketat | pin P36 #856 hijau — kontrak terkunci | **CLOSED** |
| G4 sslmode require→prefer | commit `a5046f1` | **CLOSED** (sejak pass-25) |
| UpdateSettings 1-fail pass-25 | artefak TDD mid-cycle (§4 pass-25) | **CLOSED** |

---

## 7. Dedup, Retraksi & Carry-Over

### 7.1 Retraksi

- **J15** — DIRETRAK, bukti di §5.7: `/logout` terdaftar di port (`router_full.cpp:263–264`, clear cookie di `logout.cpp:61–62`) DAN di Go (main.go:474). Tidak ada varian temuan yang selamat.
- **J14** — **premis DIRETRAK** (substansi dilanjutkan, koreksi §5.11): klaim "port tidak menyetel CSP" tidak benar — port menyetel CSP di 5 lokasi (`nginx.conf:42/:55/:78/:104/:121`). Yang benar: CSP terlalu ketat untuk JS inline miliknya (→ **I39**) dan tanpa origin Turnstile/font (→ **I24 butir 2**).

### 7.2 Mapping seri ID (catatan)

File `tests/test_p36_tdd.cpp` memakai **seri ID temuan LOW pass-24 ("Batch 3")**: D6–D13, E-b, E-d, G5–G10, F8, F11, I5, version_gate. Dokumen pass-25 meneruskan seri paralel (D6+, E-l+, G10+, F12+, I11+). Akibatnya ada **dua D8/D9/D11/D12/D13 yang berbeda makna**. Mapping yang aman (satu temuan, dua label): `P36.D10` ≈ komentar fail-open (pass-25 mencatat di exams.cpp, D21 menambah posisi `:938`); `P36.D9` ≡ J5/pengawas_state stub; `P36.F11` ≡ facet submit-key dari temuan XFF pass-25; `P36.D8` ≈ pengawas UPDATE 0-baris (pass-25 D17/D18). **Rekomendasi: di remediasi berikutnya, tulis ulang backlog memakai satu seri (tabel §3.1 dokumen ini) untuk menghentikan kebisingan.**

### 7.3 Dedup lintas-agen

- **J5 ≡ P36.D9** (stub pengawas_state) — satu temuan, sudah disatukan.
- **J9 / K9 / K8** — satu pola "baked stale strings", tiga instance berbeda (settings :2269, dashboard :1827, settings :2270); remediasi dalam satu sapuan de-bake.
- **J2 ≡ I24** (Turnstile vs CSP domain) — perbaikan CSP menutup separuh J2. §5.11 menegaskan ini **bukan hipotesis**: template port aktif meng-wire widget (`reset_password.html:90-92/:173-176`, `forgot_password.html:23`).
- **J14 → I39 + I24** — premis "port tanpa CSP" ditarik (port set CSP di 5 lokasi); substansi dilanjutkan: JS inline diblokir → I39; origin Turnstile hilang → I24 butir 2.
- **H9 ≈ M25-webui** — export XML tanpa escape (port `admin.js:1380-1424`) adalah bug yang sama dengan M25 webui (fix `428568c` meng-escape `:1360-1392`); refresh admin.js dari `90ca7de` menutup keduanya.
- **I41 ≡ M13-webui** — guard double-submit `cek_hasil`; bug pra-fix yang tetap hidup di port (§5.11.2).
- **J7 ≡ I23** (robots.txt) — route-nya di I23.
- **H8 ≡ I23** (toggle routes) — H8 sisi frontend, I23 sisi route.
- **F19** meng-subsume temuan webhook-divergence di §5.9 paritas.
- **G7 re-report** kini terkunci pin P36 (#868) — tidak dihitung dua kali di backlog.

### 7.4 Carry-over masih terbuka (dari pass-25, IDs sesuai dokumen pass-25)

- **D-area**: D8 (UPDATE 0-baris exams.cpp:706–709), D13 (exams.cpp:1152–1155), D15 (next_id fail→1 dikonflasi 409 :451/:508–523), D16 (bulk tanpa cap/dupes), D17–D20 (pengawas UPDATE 0-row→200 :392–395/:461–462; set_auto_approve absent→false :453–455; read fail-open+is_privileged :230/:297/:350/:505; pengawas_state connected:true :510). D9 (remove() true untuk DELETE 0 baris :420), D11 (restore ignored :882), D12 (comment fail-open :1243) — ketiganya kini juga terkunci pin P36. D6 pass-25: pin P36 #856 hijau — kontrak saat ini terkunci.
- **E-area**: E-t (settings.cpp:145/:154–156 protobuf hardcoded + message type salah), E-u (settings.cpp:416 verify-fail tanpa remove → orphan R2), E-n (vouchers.cpp:129 expires_at kosong→500 + WIB naive INSERT :126–129), E-q (fake-empty partials: users.cpp:277, submissions.cpp:185, vouchers.cpp:476/:536/:577, export.cpp:196, settings.cpp:441), E-o (batch 500 :366–369), E-p (dead :764–766), E-r (PQresultErrorMessage maybe-null), E-s (COMMIT fail tanpa ROLLBACK :662–663), E-b (export_submissions_csv dead :147–151 — pin P36 #864).
- **F-area**: F3 (hasil.cpp:398–402 raw token Location + tanpa sanitasi CRLF), F6 (g_idem_jobs growth :46–47), F10 (OR-gate :580), F11 (sanitize_mac_like_go :1177–1186 + presence_rate_key :355–360 + XFF mentah :372–377/:557/:852/:897/:1075/:1259/:1341 — facet submit terkunci pin P36 #872), F13 (double-enqueue :1012–1049 + g_idem_jobs), F15 (exam_result pending 200 :1180–1225), F16 (raw JSON embed :790–806 + hasil.cpp:611–612 — terkait J4).
- **G-area**: G11 (pool_real.cpp:14–21 tanpa connect_timeout), G12 (acquire tanpa cap in-flight :50–61), G13 (conninfo quoting/`+`-decode/IPv6 :74–78/:35/:68), G16 (Server::stop cross-thread server.cpp:654), G19 (execute_transaction_result latent UAF dead code :243–270), G7 (re-report, pin #868), + 12 caveat G (ping di bawah mutex, signal handler late :174–175, start condition once-only :160/:165/:170, pool_global env-snapshot, dst.).
- **I-area**: I12 (nginx.conf:87–90 limit_except no-op), I13 (compose env gap :61–78), I14–I20 (LOW), + baru pass-26: I21–I24 (§5.5), I25–I38 (§5.10).
- **H/J/K pass-26**: H8–H13, J1–J13, J16, K8–K10 — semua terbuka (§5.6–5.8); **J14 premisnya ditarik** (→ I39+I24, §5.11). Tambahan UI §5.11: I39 (MEDIUM), I40 (LOW), I41 (MEDIUM).

### 7.5 Catatan akurasi dokumen

- **P37 = 13 pin, bukan 12** — dokumen pass-25 menyebut jumlah 12; file `test_p37_tdd.cpp` berisi 13 test dan ctest menunjukkan #875–887 (13) hijau. Angka §3.3 dokumen ini yang benar.
- **K8–K10 + daftar J** direkonstruksi dari transkrip sesi (teks asli laporan agen hilang saat kompaksi konteks). Semua lokasi file:line pada bagian tersebut tetap dapat diverifikasi ulang langsung di repo.

---

*Pass-26 selesai. Remediasi berikutnya: mulai dari backlog P36 (14 pin merah §3.1) + prioritas MEDIUM §1. Pola proses yang terbukti: pin red-phase dulu → remediasi → pin hijau (P37) sebagai bukti.*
