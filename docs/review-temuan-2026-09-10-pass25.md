# Review Temuan — Pass 25 (2026-09-10)

> Final pass-25 — laporan agen D/E/F/G/I (wave 1–2) lengkap di §5; H/J/K dipotong per instruksi pengguna (§5.6–5.8).
> Sesi review bersifat review-only: tidak mengedit, tidak build, tidak commit apa pun.

## 1. Ringkasan Eksekutif

**Cakupan**: review menyeluruh pass-25 atas working tree = HEAD `a5046f1` (tree bersih 100%). Lima dari delapan area selesai ditinjau agen paralel — D (admin exams/pengawas), E (admin users/settings/vouchers/auth), F (api/webhook/hasil), G (queue/store/db/redis/server-core), I (nginx/compose/Dockerfile). Area H/J/K (frontend JS + template) tidak dijalankan — sesi diakhiri per instruksi pengguna sebelum wave-3 diluncurkan (§5.6–5.8).

**Total: 5 HIGH, 20 MEDIUM, 23 LOW baru + 2 LOW re-report (G5/G7) + 13 catatan/caveat** — ditambah 1 re-verify (F11 — label LOW pass-24, asesmen MEDIUM-leaning), 8 DIRETRAK dengan bukti, dan 45 item terverifikasi-baik lintas area.

**Dua dari lima HIGH adalah regresi yang DIINTRODUKSI remediasi pass-24 sendiri** — memvalidasi mandat pass-25 bahwa pin hijau ≠ bebas bug baru hasil refactor:

- **F12** — remediasi F1 (webhook OTP) memperkenalkan release-before-use: SETIAP webhook OTP valid berakhir "Gagal mengaktifkan akun" (UPDATE dieksekusi pada koneksi yang sudah dikembalikan ke pool) → flow registrasi→aktivasi WhatsApp rusak total, senyap tanpa crash.
- **I11** — remediasi I3 (.dockerignore) mengecualikan path yang tetap di-`COPY` Dockerfile → `docker compose build` gagal → stack produksi tidak dapat dibangun sama sekali.

Tiga HIGH lainnya: **D7** (bulk-toggle/bulk-delete lintas instansi — bypass multi-tenancy paling serius), **E-l** (instansi_update: mutasi BERHASIL commit lalu dibalas 503 selamanya — laporan gagal palsu pascamutasi), **E-m** (delete_voucher fake-200 saat PG down — voucher "terhapus" tetap redeemable).

Pola lintas-area utama:
- **Kelas release-discipline kini 3 anggota** — E-d (`submissions.cpp`), G18 (`system_apps_page`), F12 (varian inverse: release terlalu dini → exec di koneksi null) → satu remediasi RAII/scope-guard + hardening `exec_params` tolak koneksi-null.
- **Fail-closed belum tuntas** — E-q (baca fake-empty/404 saat PG down), F14 (approval fake-200), F15, I13 (env Turnstile tidak dioper compose → mati senyap di produksi), G17 (health selalu "ok").
- **Durabilitas antrean & pool** — G14 (job hilang tanpa jejak saat Redis mati penuh), G15 (restart Redis = worker mati permanen seumur proses), G12/G13 (pool tanpa cap in-flight / conninfo tanpa escaping).

**Prioritas remediasi disarankan**: (1) F12 + I11 — pulihkan fungsi inti & deployability; (2) D7 — tenancy; (3) E-l, E-m, F14 — fail-closed; (4) kelas RAII release (E-d + G18 + hardening F12); (5) G14/G15/G17; sisanya MEDIUM/LOW menyusul.

**Baseline**: 855 test / 203 suite — 847 PASS, 7 SKIP (6 PgIntegration butuh `DATABASE_URL` riil; `P7_Frontend.JsGuardCount`), 1 FAIL (`UpdateSettings_JsonStillWorks`) CLOSED sebagai artefak TDD mid-cycle (§4) — rebuild terisolasi 1/1 PASSED; kedua target build hijau (§3).

## 2. Baseline & Timeline

**Kondisi tree**: HEAD `a5046f1`, `git status --short` kosong — tree bersih 100%, termasuk file yang tadinya untracked (`.dockerignore`, doc pass-24, `tests/test_p32_tdd.cpp`…`test_p35_tdd.cpp`, `tests/test_pg_integration_tdd.cpp`). Review pass-25 menargetkan isi working tree — identik dengan kondisi committed.

Dua commit baru (sesi editor paralel, semalam–dini hari):
- `a3d9acd` `fix(review-pass24): TDD remediasi HIGH+MEDIUM — fail-closed admin-write, queue durability, stack-pool family` — seluruh remediasi overnight (Batch-1 HIGH P33, Batch-2 MEDIUM P34, Batch-2b P35) + file test pin + `.dockerignore` + doc pass-24.
- `a5046f1` `fix(db): koreksi default sslmode require → prefer (G4 P36-koreksi)` — default `sslmode=require` memutus stack compose produksi (image `postgres:16-alpine` ber-`ssl=off`, koneksi ditolak senyap) → default diubah ke `prefer`; kepatuhan TLS tetap dicapai via `sslmode` eksplisit di URL klien. Test diamended (P36-koreksi).

| Waktu (2026-09-10) | Peristiwa |
|---|---|
| 03:13:54 | `src/main.cpp` terakhir diedit (blob `5eba9b24aa461579f1d0b217855a8128471d56b0`) — live-fix build-break pasca-refactor G9 |
| 03:20 | Baseline build+test (task `bzxgaur19`): examvan-server **BUILD FAILED** @`main.cpp:170` (`db.ping()` tak dideklarasikan); examvan-tests **855 test / 203 suite — 847 PASS, 7 SKIP, 1 FAIL** |
| 03:21:43 | Fingerprint `git diff HEAD` = `54d4908b72aaa553a70a5b071b00fa66832b9dd244b6de58a2546fe91317bf17` — 33 file berubah (+1031/−491); remediasi overnight **belum** di-commit |
| 03:27:51 | `src/handlers/admin/settings.cpp` mtime terakhir (blob `15c3468e34c2fd3c92a1038444935bd96322ffb3`) |
| 03:31:44 | `tests/test_protobuf_handlers_tdd.cpp` mtime terakhir (blob `cc9e9d05b04b534a5a41f6c52bce958f8f6368ef`) |
| 03:21–03:54 | Sesi paralel me-commit `a3d9acd` lalu `a5046f1` |
| 03:54:04 | `git diff HEAD` **kosong** — sha256 `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` (hash string kosong) |
| 03:54–03:56 | Verifikasi terisolasi (task `bup18aoa7`, dir `/tmp/build-pass25` — tidak menyentuh `build/` milik sesi paralel) |

## 3. Status Build & Test (verifikasi terisolasi `bup18aoa7`, 03:56)

- `examvan-tests`: **BUILD OK** (TESTS_BUILD_EXIT=0)
- `examvan-server`: **BUILD OK** (SRV_BUILD_EXIT=0) — live-fix `main.cpp:170` terkonfirmasi kompilasi: `bool db_up = pool && pool->ping();` (:85) + `if(db_up){ expiry.start(); cleanup.start(); retention.start(); }` (:170)
- Single-test `--gtest_filter='ProtobufHandlers.UpdateSettings_JsonStillWorks'` → `[ OK ] (0 ms)` — **1/1 PASSED** (03:56:16)

Baseline 03:20 examvan-tests: 855 test / 203 suite — **847 PASS**, **7 SKIP** (`P7_Frontend.JsGuardCount`; `PgIntegration.MigrateCreatesAllHandlerTablesOnFreshDatabase`; `PgIntegration.FreshDbSchemaFitsProductionRegisterInsert`; `PgIntegration.SharedDbMigrateIsIdempotentAndReady`; `PgIntegration.RegisterHandlerWritesUserStore`; `PgIntegration.VoucherRedeemEndToEndViaHandler`; `PgIntegration.CreateExamPersistsToPostgresStore` — 6 terakhir butuh `DATABASE_URL` riil), **1 FAIL** (`ProtobufHandlers.UpdateSettings_JsonStillWorks` — §4, CLOSED).

## 4. Root-Cause: `UpdateSettings_JsonStillWorks` gagal @03:20 — CLOSED (artefak TDD mid-cycle, bukan bug)

**Test yang diamended** (`tests/test_protobuf_handlers_tdd.cpp:312-326`):
1. Body kosong → `update_settings` wajib **400** (bebas DB).
2. `DATABASE_URL` di-set ke host mati (`postgresql://…@127.0.0.1:59999/…`) + body valid (`{"smtp_host":"smtp.test.local"}`) → wajib **503** + body mengandung `"Database tidak tersedia"`.

Catatan urutan env dalam suite: `RedeemVoucher_JsonStillWorks` (±:262) me-unset `DATABASE_URL` dan tidak ada yang me-reset-nya sebelum `UpdateSettings` → assertion body-kosong berjalan di mode memori, mengharapkan 400 yang tak bergantung DB — lalu test me-set host-mati untuk kasus 503 fail-closed.

**Kode saat ini** (`src/handlers/admin/settings.cpp:236-295`, blob `15c3468e…`):
- `:271-273` — `pending.empty()` → 400 `"tidak ada setting yang valid"` — **dieksekusi sebelum gate PG**.
- `:284-286` — upsert gagal → 503 + `"Database tidak tersedia"`.

→ Kode memenuhi kedua assertion test.

**Penjelasan kegagalan 03:20**: binary test yang berjalan di baseline dikompilasi di tengah siklus TDD sesi paralel — ekspektasi baru sudah masuk binary, tetapi cabang empty-body→400 di `settings.cpp` belum mendarat saat binary itu dibangun (`settings.cpp` selesai 03:27:51, file test diamended lagi 03:31:44 — keduanya SETELAH binary baseline dikompilasi). Kegagalan = binary basi, bukan regresi kode.

**Konfirmasi empiris (03:56)**: rebuild terisolasi penuh (configure + kedua target, `/tmp/build-pass25`) → build hijau; single-test pada blob `settings.cpp` `15c3468e…` → `[ OK ] (0 ms)`, **1/1 PASSED**, exit 0.

**Status**: CLOSED — bukan temuan; tidak ada remediasi yang dibutuhkan.

## 5. Temuan

*(ID meneruskan seri masing-masing: D6+, E-l+, G10+, F12+, I11+; H/J/K meneruskan seri pass-24. Item tipis pass-24 yang di-re-verify: E-i/E-j/E-k (agen E), F11 (agen F), I4/I6–I10 (agen I). Area H/J/K dipotong di pass-24 dan TIDAK dijalankan di pass-25 — sesi diakhiri per instruksi pengguna sebelum wave-3 diluncurkan; disposisi di §5.6–5.8.)*

### 5.1 Area D — admin exams/pengawas *(laporan agen D — MASUK)*

**14 temuan baru: 1 HIGH, 4 MEDIUM, 9 LOW.** Satu draf awal DIRETRAK dengan bukti: **D14** ("early return bad-status tanpa release → pool exhaustion") salah — `RealPool::acquire()` mengembalikan `PgConnPtr` = `unique_ptr` + deleter `PQfinish` (`src/db/pool_real.cpp:50-70`) → RAII-aman, bukan leak (`exams.cpp:1514-1516`, `exam_store_postgres.cpp:431-432`).

#### HIGH

**D7 — HIGH — Scope operator bulk-toggle/bulk-delete tidak memeriksa instansi PEMILIK ujian**
`src/handlers/admin/exams.cpp:1270-1273` (dipanggil dari `bulk_toggle_exams` :1314 dan `bulk_delete_exams` :1366). JOIN `admin_users owner ON owner.id=$2` hanya memastikan baris creator ADA — `owner.instansi` tidak pernah dibandingkan dengan `me.instansi`; kondisi `me.instansi=$3` (instansi aktor sendiri) trivially true. Route bulk terdaftar TANPA scope (`src/http/router_full.cpp:421-422`, `admin_api(handler)` polos) → handler adalah satu-satunya pertahanan — kontras dengan guard `exam_access` router (`router_full.cpp:170`) yang BENAR memuat `me.instansi=owner.instansi`.
**Dampak**: operator instansi mana pun (non-kosong, non-personal) dapat bulk-toggle & bulk-DELETE ujian milik instansi LAIN — bypass multi-tenancy lintas sekolah/tenant paling serius di area ini.
**Fix**: tambah `AND owner.instansi=me.instansi` pada SQL; dan/atau daftarkan route bulk dengan scope `exam_access` seperti route pengawas per-exam.

#### MEDIUM

**D6 — MEDIUM — create_exam path sukses protobuf tidak finalize_idempotency** — `exams.cpp:526-537` vs path JSON `:542-548` (yang benar). Path protobuf return 201 TANPA `finalize_idempotency` → row tetap `state='pending'` → retry dengan Idempotency-Key sama = `reserve_idempotency` Conflict (`exam_store_postgres.cpp:449-461`) → 409 padahal ujian SUDAH dibuat — key teracun permanen. Semua jalur ERROR protobuf sudah diremediasi P33-D1; jalur SUKSES terlewat (test_p33 tidak mem-pin path ini — hanya early-ack removal :146/:153). Fix: `finalize_idempotency(idem, idem_fingerprint, 201, out, "application/x-protobuf")` sebelum return.

**D8 — MEDIUM — edit-token: kegagalan `update()` PG dikonflasi jadi 404** — `exams.cpp:706-710`. `update()` false tidak hanya saat id hilang — juga saat UNIQUE violation token (race lintas proses setelah `claim_token_if_absent` lolos cek SELECT non-persisting) maupun error SQL/koneksi → admin lihat 404 untuk ujian yang jelas ada; race token nyata tak pernah dilaporkan 409 DUPLICATE_TOKEN. Fix: bedakan 0-baris (via `PQcmdTuples`) dari error SQL (409/503).

**D13 — MEDIUM — save_exam_questions: field kosong me-RESET nilai tersimpan** — `exams.cpp:1184-1193`. `panel_color`/`start_time`/`end_time`/`congrats_message` memakai semantik "absent = reset" (`.reset()`) sedangkan identity/questions/security_level if-present → partial update (mis. API client hanya mengirim questions) diam-diam MENGHAPUS warna panel, jadwal, dan pesan congrats ujian aktif. Fix: bedakan absent (jaga nilai lama) vs explicitly-empty (reset).

**D17 — MEDIUM — pengawas: UPDATE 0-baris tetap dibalas 200 sukses** — `pengawas.cpp:392-395` (set_approval), `:461-462` (set_auto_approve). `PGRES_COMMAND_OK` true untuk UPDATE 0 baris → `exam_id`/MAC salah tetap 200 `"Persetujuan diperbarui"`/`"Auto-approve diperbarui"`; bug di sisi client (salah ketik MAC) tak pernah ketahuan. Akar sama dengan D9. Fix: `PQcmdTuples=="1"` → 404/400 bila 0.

#### LOW

**D9 — LOW — `ExamStorePostgres::remove()` true untuk DELETE 0-baris** — `exam_store_postgres.cpp:420` + `exec_command:173-177`. Paritas dengan memory (false saat id absen) pecah: cabang 404 `delete_exam` (`exams.cpp:926`) unreachable di PG; double-delete concurrent → dua 200; bulk ok_count menghitung id sudah-terhapus. Fix: `PQcmdTuples=="1"`.

**D10 — LOW — Orphan objek R2**: kegagalan cleanup hanya di-log (`exams.cpp:938`, tetap 200) + objek LAMA tidak pernah dihapus saat edit PDF (`:865-885`, upload key baru tanpa `remove` key lama) → akumulasi objek R2 tak terjangkau API → biaya storage. Fix: metadata kegagalan di response (atau queue cleanup) + remove key lama setelah update sukses.

**D11 — LOW — update_exam: hasil restore R2 diabaikan** (`exams.cpp:882`) — bila `update()` gagal setelah upload PDF baru, restore dijalankan tapi return value-nya tidak diperiksa → restore gagal tak tercatat, objek baru jadi orphan tanpa penanda. Fix: log/catat bila restore gagal.

**D12 — LOW — Komentar "fail-open" vs kode fail-closed di `exam_bulk_scope_ok`** (`exams.cpp:1243`) — PG terkonfigurasi tapi unreachable → semua scope false → item di-skip diam-diam → 200 `"updated":0` yang mengklaim sukses; operator tak bisa membedakan "tidak ada izin" dari "DB down". Fix: 503 saat PG unreachable vs 200 updated:0 saat scope memang menolak.

**D15 — LOW — `next_id()` gagal → 1 → `add()` gagal → dikonflasi 409 DUPLICATE_TOKEN** — `exams.cpp:451/508-523` + `exam_store_postgres.cpp:363-368`. Semua kegagalan add() (termasuk FK `created_by=0`, koneksi mati) dibalas 409 DUPLICATE_TOKEN (error asli hanya di log); cleanup R2 lalu dijalankan dengan key id=1 yang salah. Fix: pisahkan error insert dari collision token; batal/catat cleanup bila id invalid.

**D16 — LOW — Bulk: ids tanpa cap, duplikat dihitung 2×, audit `"keys=2"` konstan** — `exams.cpp:1295/:1338` (parse_int_array tanpa batas; loop sekuensial R2+PG per item), `:1372-1373` (id duplikat: remove() 0-baris tetap true via D9 → ok_count double), `:1383` (`keys=` selalu 2, bukan jumlah keberhasilan). Fix: cap array (mis. 100), dedup ids, audit informatif.

**D18 — LOW — set_auto_approve: `enabled` absent/invalid diam-diam jadi false** — `pengawas.cpp:453-455` (`"true"/"1"` → true, selain itu false, tanpa 400) → typo ("tru") atau field terlupa mematikan auto-approve tanpa feedback validasi. Fix: 400 untuk nilai non-boolean.

**D19 — LOW — Endpoint baca pengawas fail-open + `is_privileged:true` hardcoded di fallback** — `pengawas.cpp:230` (juga :297, :350, :505): DB down → 200 array kosong (tanpa 503 untuk membedakan "tidak ada data" dari "DB down"); fallback `pengawas_exams` meng-hardcode `"is_privileged":true` bagi siapa pun yang gagal → UI salah menampilkan status privilege. Fix: 503 saat DATABASE_URL terkonfigurasi (paritas P18-M9/P20-M9 pada mutasi); `is_privileged` dihitung, bukan literal.

**D20 — LOW — `pengawas_state` stub selalu `"connected":true`, data nol** (`pengawas.cpp:510`) — dashboard pengawas klaim sehat terlepas status DB/Redis. Fix: implementasi nyata atau 501.

#### Terverifikasi baik (agen D, dengan bukti)

1. **Parameterisasi SQL pengawas penuh** — semua query `$N` (`pengawas.cpp:141-166` list dinamis, :234-298, :301-351, :373-413, :446-480, :482-506); satu-satunya konkatenasi `std::to_string(uid)` di block stats (int, aman).
2. **Rantai auth router pengawas per-exam** — rate limit → session dual-key → admin_id>0 → revalidasi PG session → role gate → CSRF (mutasi) → scope `exam_access` (`router_full.cpp:444-449`) → internal header dari session terverifikasi. Guard `exam_access` (:170) memuat `me.instansi=owner.instansi` yang benar — **D7 adalah outlier, bukan pola sistemik**.
3. **Implementasi idempotency store bersih** — reserve (INSERT ON CONFLICT DO NOTHING + `PQcmdTuples=="1"` → New; completed+fingerprint → Replay; selain itu Conflict), finalize (UPDATE + upsert fallback), release (DELETE) di `exam_store_postgres.cpp:424-505`; **DDL fresh-DB LENGKAP** (ALTER `:315-323` `IF NOT EXISTS` menambah state/response_*/timestamps) — dugaan mismatch skema fresh-DB **REFUTASI**. Cacat hanya di sisi handler (D6).
4. **Mutasi pengawas fail-closed** — 503 saat DATABASE_URL terkonfigurasi (set_approval :403-413, set_auto_approve :470-480); whitelist status approval 400; `json_escape_pw` semua output; clamp pagination; audit B1 atribusi benar.
5. **Transaksi `delegate_exam`** — per-statement pg_ok/guru_invalid/pengawas_invalid, validasi target same-instansi+active+guru, `real.release(c.release())` di setiap exit (`exams.cpp:1505-1530`).
6. **Ordering delete/update vs R2 benar** — delete: store-first `remove` :926 → R2 :930-941 (PDF tidak orphan saat remove gagal); edit: `update` sebelum `upload` :865-885 (P35-D4 benar); bulk_delete: 503 SEBELUM mutasi bila R2 tidak siap (:1352-1363). Sisa masalah hanya D10/D11.
7. **`release_and_return` exactly-once di ketiga jalur error protobuf** — tiap path mem-bungkus lalu return segera; tidak ada double-release maupun leak. Satu pengecualian: jalur sukses protobuf (→ D6).

### 5.2 Area E — admin users/settings/vouchers/auth *(laporan agen E — MASUK)*

**2 HIGH, 4 MEDIUM, 4 LOW + 1 caveat.** Task-1 sanity-check `update_settings`: **KONFIRMASI BENAR** (validasi 400 di `settings.cpp:271-273` berjalan sebelum gate PG `:278-292`). Keempat kandidat koordinator (a-d) **semua VALID dengan bukti baris**. Header laporan agen menyebut HEAD `390fd0b` (bacaan git-log basi) — file handler area E dibaca dari DISK; temuan berlaku untuk tree saat ini (remediasi overnight tidak menyentuh `users.cpp`/`vouchers.cpp`/`auth_store.cpp`; `settings.cpp` tersentuh E-g dan tetap diverifikasi ulang per baris).

#### HIGH

**E-l — HIGH — `instansi_update` selalu 503 di mode PG: fitur 100% rusak + laporan gagal palsu SETELAH mutasi BERHASIL** — `src/handlers/admin/users.cpp:689-710`. Lambda `with_pg` `:697-703` menjalankan `UPDATE admin_users SET instansi=$2` (:700), log sukses (:701), release (:702) — TANPA sentinel hasil; lalu `:708` `if(pg_configured_from_env())` mengembalikan **503 "Database tidak tersedia" secara tidak bersyarat**. Mekanisme: request valid saat DATABASE_URL aktif → UPDATE COMMIT di DB → response 503 → admin mengira gagal, retry tetap 503 selamanya. Komentar `:706-707` ("P33-Ea: PG tidak terjangkau → fail-closed") tidak sesuai implementasi — gate harus menandai apakah lambda benar-benar dieksekusi/sukses (pola sentinel `edit_user`), bukan hanya cek `pg_configured_from_env()`. File ini tidak tersentuh remediasi semalam — kandidat TDD remediasi berikutnya.

**E-m — HIGH — `delete_voucher`: satu-satunya jalur tulis voucher TANPA gate 503 → fake-200 saat PG down** — `src/handlers/admin/vouchers.cpp:415-431`. Kontras: `delete_submission` MEMILIKI gate (`submissions.cpp:311-318`); `create_voucher` `:318-322`, `toggle` `:409`, `redeem` `:674`, `activate` `:728` semuanya fail-closed (sisa P33-Eh). Saat PG dikonfigurasi tapi unreachable: `with_pg` no-op (lambda return awal `:422` tanpa mengisi `result`) → `result` tetap `""` → lolos cek `:428`/`:429` → `:431` **200 `{"success":true,...,"message":"Voucher dihapus"}`**. Mekanisme: POST delete saat outage → 200 sukses palsu → baris TIDAK terhapus → voucher tetap aktif & redeemable, UI admin menampilkan "terhapus". Kelas P20-M9 fake-200. Fix: gate 503 + sentinel (tinggal menyalin pola `delete_submission :311-318`).

#### MEDIUM

**E-t — MEDIUM — `settings_page` jalur protobuf: Settings HARDCODED + tipe pesan SALAH** — `src/handlers/admin/settings.cpp:144-160`, dijangkau via `GET /admin/api/saas-settings` (`router_full.cpp:387`, superadmin, GET dikecualikan CSRF) dengan `Accept: application/x-protobuf` (`middleware/protobuf.cpp:21-25`). `:152-160` (saas-settings) mengembalikan `examvan::v1::Settings` HARDCODED (`:154-157`: `smtp.gmail.com`, port 587, `default_max_exams=3`, `default_max_pdf_size_mb=1`) mengabaikan `saas_settings` PG — sedangkan jalur JSON `:161-205` membaca PG benar → klien Android x-protobuf melihat setting fiktif (kuota ujian dilaporkan 3 padahal beda). `:144-148` (system-apps) mengembalikan tipe pesan SALAH (`examvan::v1::VoucherList` kosong + `set_success(true)`) → klien protobuf yang mengharapkan SystemApp gagal parse. Payload hardcoded TIDAK berisi kredensial (tidak ada `smtp_password`) — dampak data palsu/pelanggaran kontrak, bukan kebocoran rahasia. Fix: baca `load_all_settings()` untuk kedua jalur protobuf + tipe pesan benar.

**E-u — MEDIUM — system-apps upload: `verify(key)` gagal → 502 TANPA `client.remove(key)` → orphan R2** — `src/handlers/admin/settings.cpp:410`. Upload R2 OK tapi verify gagal → return 502 langsung → objek orphan hingga 100MB per kejadian (body ≤105MB via carve-out I1); asimetris dengan `:412` (kegagalan INSERT → `remove` dipanggil). Fix: "upload ATAU verify gagal → remove(key) lalu 502".

**E-n — MEDIUM — `create_voucher` dengan `expires_at` kosong → 500 di mode PG** — `vouchers.cpp:128-129` (`valid_expires_at`: string kosong → `true`, maksudnya voucher tanpa batas) tapi `:299-312` meneruskan string kosong mentah ke `$5::timestamptz` (`:300`) → PG error "invalid input syntax for type timestamp" → `__fail__` → 500 "Gagal menyimpan voucher" (`:312`). Admin kirim form tanpa tanggal → 500; voucher tanpa batas waktu tidak bisa dibuat. Fix: kosong → `std::nullopt` via `exec_params_nullable` (tersedia `pool_real.cpp:77-83`).

**E-q — MEDIUM — keluarga read fake-empty saat PG-down (M9-family sisi baca)** — handler baca mengembalikan 200 data-kosong saat PG dikonfigurasi tapi unreachable; instance terparah `user_detail` mengonflasi DB-down dengan **404 "user tidak ditemukan"** (bisa memicu tindakan admin salah, mis. membuat ulang user). Gate 503 P20-M9 diaplikasikan ke TULIS tapi tidak ke BACA. Lokasi (semua `src/handlers/admin/`): `users.cpp:277` (list `"users":[]` saat `!got`), `users.cpp:280-306` (user_detail → 404), `vouchers.cpp:249` (list_vouchers), `vouchers.cpp:469` (voucher_redemptions), `vouchers.cpp:529` (list_audit_logs), `vouchers.cpp:570` (vouchers_mine), `vouchers.cpp:759` (packages_json), `settings.cpp:434-435` (system-apps GET — kandidat c koordinator), `submissions.cpp:185` (list_submissions empty), `export.cpp:196` (catch-all → XLSX hanya header, ekspor kosong tanpa 503). Fix: 503 saat `DATABASE_URL` terkonfigurasi + PG unreachable.

#### LOW

**E-o — LOW — `create_voucher_batch` DB-down → 500 bukan 503** (`vouchers.cpp:366-369`) — inkonsisten dengan konvensi P33-Eh di create/toggle/redeem/activate; klien retry-backoff/monitoring memperlakukan 500 ≠ 503.

**E-p — LOW — dead code** (`vouchers.cpp:764-766`): `middleware::is_protobuf_accept(Request{})` — `Request{}` lokal tanpa header selalu false; cabang tak pernah aktif. Bersihkan.

**E-r — LOW (informasional) — `PQresultErrorMessage` dengan argumen mungkin-null** — `PQexecParams` bisa return null saat koneksi putus; dipanggil pada `unique_ptr` mungkin-null di `users.cpp:392/:485/:525/:569/:605/:635/:674`, `submissions.cpp:304`, `vouchers.cpp:307/:425`. BUKAN runtime bug: libpq modern memeriksa NULL dan mengembalikan string kosong — kebersihan defensif saja, tidak mendesak.

**E-s — LOW — redeem voucher: COMMIT gagal → release TANPA ROLLBACK** (`vouchers.cpp:662-663`) — koneksi masih CONNECTION_OK (koneksinya baik, transaksinya gagal) → kembali ke `idle_` pool dalam kondisi aborted-transaction → pemakai pool berikutnya untuk koneksi itu mendapat "current transaction is aborted" pada statement pertamanya. Fix: ROLLBACK best-effort sebelum release.

**Caveat timezone — LOW** (`vouchers.cpp:128-147`, komentar menyatakan input WIB): string naif "YYYY-MM-DD HH:MM" diteruskan mentah ke `$N::timestamptz` → PG menafsirkan per zona sesi server; bila TimeZone server ≠ Asia/Jakarta, kedaluwarsa bergeser ±7 jam. Sisi baca aman (`parse_pg_or_iso_utc` `utils.cpp:40-99` menangani `+07`). Fix: offset eksplisit "+07" atau `SET TimeZone` per koneksi.

#### Item pass-24 tipis — disposisi

- **E-i/E-j/E-k — UNMAPPABLE, diperlakukan closed-by-rescan**: detail aslinya hilang saat kompaksi pass-24 (doc pass-24 `:269`/`:382` hanya menyimpan label+severity). Audit segar pass-25 ini menutupi file yang sama secara menyeluruh (users/vouchers/settings/auth_store/login/password) dan menemukan E-l..E-u dengan bukti baris — tidak ada celah yang diketahui tertinggal tanpa di-scan. Bila transkrip agen E pass-24 ditemukan, silakan cross-check; praktisnya remediasi berikutnya cukup menargetkan temuan pass-25.
- **E-e — RESOLVED** (dianulir agent E pass-24 sendiri, doc `:269`).
- **E-b — MASIH TERBUKA**: `export_submissions_csv` (`export.cpp:147-151`) tetap dead code — router hanya menyambungkan `export_xlsx` (`router_full.cpp:435`/`:438`). `export_xlsx` sendiri baik (with_global_pg, LIMIT 50k, scope `allowed_ids`, release `:194`) kecuali catch-all `:196` → instance E-q.
- **E-d — MASIH TERBUKA, diperluas**: early-return tanpa release di `submissions.cpp:212-217`/`:296-301` (scope-deny paths) — **SATU KELAS dengan G18** (settings.cpp `system_apps_page`): deleter `PgConnPtr` = `PQfinish` → koneksi sehat ditutup, tidak dikembalikan ke `idle_` → churn TCP+auth per request; RealPool tanpa accounting outstanding. Remediasi tunggal RAII/scope-guard untuk kedua file.

#### Terverifikasi baik (agen E, dengan bukti)

1. **E-a remediasi BAIK** — `create`/`edit`/`delete`/`toggle_status`/`verify`/`deactivate_package`/`change_password` semua fail-closed 503, parameterized, sentinel benar. Sisa: E-l + instance E-q di users.cpp:277/:280-306.
2. **E-f remediasi BAIK** — `parse_pg_or_iso_utc` (`utils.cpp:40-99`) ketat fail-closed: ≥19 char, validasi rentang digit + hari-per-bulan + kabisat (`:87-91`), `.fff`→ms, offset `Z|+HH[[:]MM]`, tanpa offset = UTC, penyimpangan → nullopt → voucher TIDAK usable. Boundary `voucher_usable`: `==now` masih usable (tidak ada off-by-one). Sisa: E-n + caveat timezone.
3. **E-h remediasi BAIK kecuali delete** — create `:320`, toggle `:409`, redeem `:674`, activate `:728` fail-closed; batch → E-o; **hanya `delete_voucher` tanpa gate → E-m**.
4. **Auth: TANPA temuan baru** — `auth_store.cpp` (445 baris): semua operasi PG parameterized (termasuk `kSelectUserSql :176-179` `LOWER(username)=LOWER($1)`), `with_global_pg` (P33), `real.release()` di SEMUA jalur sukses (`:196/:217/:238/:281/:305/:326/:351/:373/:393/:430`), `insert_registered_user :246-289` (INSERT 12 param, cek status, sentinel + fallback mem), `bump_otp_attempts :334-357` (RETURNING atomik), `get_setting :404-441` (TTL 5s, reload di luar lock). `password.cpp:33-44` compare waktu-konstan (`volatile` accumulator + cek panjang + awalan `$2b$/$2a$/$2y$`). `login.cpp:116-281`: CSRF double-submit `:120-145`, Turnstile `:146-162`, lockout 5/15min per username ternormalisasi DICEK SEBELUM verifikasi `:180-190`, dummy-hash equalization user tak-dikenal `:196-200`, gate login `status=='active'` `:219-223` (suspended/pending_otp ditolak login DAN per-request oleh `admin_api`), redirect `next` tolak CRLF `:270-278`, cookie HttpOnly+Secure (non-dev).
5. **Tidak ada kredensial `.env` yang dibaca/dicetak** — aturan keamanan dipatuhi agen.

### 5.3 Area F — api/webhook/hasil

Cakupan agen F: `src/handlers/api/webhook.cpp`, `src/handlers/api/exams.cpp`, `src/handlers/public/hasil.cpp` + audit rute. **2 temuan baru (1 HIGH + 1 MEDIUM) + 1 MEDIUM + 2 LOW + 1 re-verify.**

**F12 — HIGH — Webhook OTP: release-before-use → UPDATE aktivasi tidak pernah dieksekusi (aktivasi OTP 100% gagal di jalur PG).**
`webhook.cpp:187` — cabang user-ditemukan melakukan `c.release()` prematur (koneksi dikembalikan ke pool SEBELUM tahap UPDATE; `release()` mengosongkan PgConnPtr), lalu `:215-216` mengeksekusi `real.exec_params(c.get(), "UPDATE admin_users SET status='active', otp_code=NULL, otp_expiry=NULL WHERE LOWER(username)=LOWER($1)", {username})` pada koneksi null → PQexecParams(nullptr) mengembalikan null tanpa crash → cabang sukses `:218-221` tak pernah tercapai → `respond_channel(false, "Gagal mengaktifkan akun")`. Runtutan penuh: SELECT per-user `:174` → release saat no-tuples `:179` (OK) → seluruh pemeriksaan `:189-213` berjalan tanpa koneksi. **Dampak**: SETIAP webhook OTP valid berakhir gagal; akun tidak pernah teraktivasi; flow registrasi→aktivasi WhatsApp rusak total dengan pesan menyesatkan; senyap (tanpa crash/leak) sehingga lolos dari test tanpa PG riil. **REGRESI remediasi pass-24**: bug ini diintroduksi oleh remediasi F1 (webhook) pass-24 sendiri. **Varian INVERSE dari kelas release-discipline E-d/G18** (di sana release terlambat → koneksi sehat mati; di sini release terlalu dini → exec di null). **Fix**: satu release tunggal setelah UPDATE (hapus `:187`); atau panggil `auth::activate_registered_user(username)` (`auth_store.cpp:313-332` — UPDATE identik dengan disiplin release benar di `:326`). Hardening kelas: `exec_params` wajib menolak koneksi-null (`pool_real.cpp:70-75`).

**F13 — MEDIUM — submit_exam: idempotency check-then-set dalam dua critical-section terpisah → race double-enqueue.**
`exams.cpp:~971-992` (lock #1 + find job existing by exam+mac+exam_number → 202 dengan job_id lama) dan `:~1007-1010` (lock #2 + set map) — sementara persist (fail-closed 503 `~:995-998`) + enqueue (`~:1000-1005`) berjalan DI ANTARA keduanya. Map `g_idem_jobs` process-local (`:46-47`). Dua submit duplikat konkuren (retry-storm Android, pola C4) sama-sama lolos fase find → keduanya persist+enqueue → double grading. Pengecilan: race intra-process nyata antar dua thread uWS; lintas-replica map memang tak berlaku. Catatan positif: jalur SUKSES memang menset map (beda dari D6). **Fix**: satu critical-section menaungi reservasi slot (set "in-progress" SEBELUM persist, rollback saat gagal); atau striped per-key lock; atau pindahkan idempotency ke DB (unique constraint + `INSERT ... ON CONFLICT DO NOTHING RETURNING` seperti `submission_queue.cpp:456`).

**F14 — MEDIUM — request_approval: PG down → 200 "approved" yang tidak pernah dipersist (fake success; keluarga M9 sisi API).**
`exams.cpp:572-620` — `if(auto c = real.acquire())` melewati persist diam-diam saat null; catch `:619` berkomentar "best-effort". Status dihitung in-memory dari auto_approve (`:567-570`); INSERT exam_approvals + advisory-lock cap + COMMIT semua terlewati TANPA mengubah status → tetap 200 `{"success":true,"status":"approved"}` (+ varian protobuf). Kontras dengan idiom benar di file yang sama: `persist_submission_pending` `:192-215` (flag pg_used + 503). **Dampak**: siswa diberi tahu "approved" tanpa baris approval; gate `device_approved` (`:400-422`) fail-closed → saat submit siswa terblokir 401/404 dengan pesan kontradiktif. **Fix**: idiom pg_used; 503 (atau eksplisit 200 "pending" + `persisted:false`).

**F15 — LOW — exam_result: PG down pada fallback DB → 200 "pending".**
`exams.cpp:1141-1171` — fallback `if(auto c=real.acquire())` skip → `:1170-1171` menjawab 200 `{"status":"pending"}`. Pengecilan: jalur Redis-first `:1080-1138` berjalan lebih dulu; submit sendiri sudah fail-closed. **Fix**: flag pg_used; 503 atau `db_unavailable:true`.

**F16 — LOW — Embed JSON mentah dari string kontrol-guru tanpa validasi/escape.**
`exams.cpp:790-791`/`:804` (identity_fields dari DB), `:806` (questions pasca-strip); `hasil.cpp:611-612` (embed mentah questions_out + identity_fields ke HTML). String dari kolom exam_questions/exam_settings (kontrol guru via admin SaveQuestions) diembed verbatim tanpa json_escape/cek well-formed. Sisi HTML: kontaminasi markup halaman hasil bila berisi `<script>` — namun butuh guru aktif menyisipkan payload → LOW (single-tenant-per-exam). **Fix**: parse+re-serialize sebelum embed; escape sisi HTML (pola `helpers::json_escape`/`escape_like` `:547`).

**F11 — RE-VERIFY (pass-24 LOW; asesmen pass-25: MEDIUM-leaning) — sanitize_mac permissive + bucket per-IP raw-XFF → semua rate-limit per-device bisa dilewati.**
`exams.cpp:1177-1186` `sanitize_mac_like_go` (filter charset alnum + `: . - _`, cap 100, kosong→"unknown" — BUKAN validator MAC; "AAAA" lolos; pergeseran baris dari `:1162-1171` pass-24 akibat remediasi). `presence_rate_key` `:355-360`: mac kosong/"unknown" → `prefix+exam_id+":ip:"+ip` (ip = nilai XFF MENTAH); selain itu `prefix+exam_id+":"+mac`. Raw XFF dipakai di `:557` (request_approval), `:852` (exam_pdf), `:897` (submit_exam), `:1075` (exam_result), `:1259` (access_log), `:1341` (complete_exam). **Dampak**: (1) semua bucket per-device (submit 10/menit, reqapp-device 30/menit, pdf 10/menit, result 60/menit, presence) dikunci pada mac buatan-klien — rotasi mac sampah = bucket segar tak terbatas; (2) bucket per-IP dikunci pada XFF mentah — spoof bebas; (3) tiap mac unik = baris submissions terpisah via ON CONFLICT (exam_id, mac_address) → banjir baris + antrean grading. Pengecilan: OR-gate `:915`/`:563` menuntut approval per-mac, jadi banjir per-mac-baru butuh approval per-mac-baru; tapi reqapp-device 30/menit per-mac juga bisa dirotasi → exam auto_approve = massal approval. **Fix**: (1) parsing XFF trusted-proxy; (2) validasi format MAC riil (6 pasang hex) atau kollapse mac non-format ke satu bucket per-IP; (3) kunci bucket device pada id baris approval sisi-server.

**DIRETRAK dengan bukti (2)**:
- **F17** (job_id tidak ter-update saat resubmit) — REFUTED: pass persist worker meng-upsert job_id tiap putaran: `submission_queue.cpp:451` `UPDATE ... SET job_id=$1 WHERE id=(SELECT id FROM submissions WHERE exam_id=$.. AND mac_address=.. ORDER BY created_at DESC LIMIT 1) RETURNING id` dan `:456` `INSERT ... ON CONFLICT (exam_id, mac_address) DO UPDATE SET job_id=EXCLUDED.job_id`.
- **Webhook expiry_epoch==0 bypass** (`:194`) — REFUTED: OTP disabled/expired SELALU punya otp_code kosong (`update_user_otp` `auth_store.cpp:301`; `activate_registered_user` `:323` set otp_code=NULL) → kegagalan cocok kode terjadi lebih dulu. Sisa catatan higienis saja.

**Item terbuka — anotasi baru**:
- **F3 (CRLF injection)** MASIH TERBUKA + anotasi: `hasil.cpp:398-401` token di-raw-uppercase ke header Location 302 — CRLF-injection hidup di KEDUA jalur emisi: posix `server.cpp:88-96` (`for (auto& [k,v] : r.headers) ss << k << ": " << v << "\r\n";`) dan uWS `server.cpp:544-545`; fix di titik emisi.
- **F6 (g_idem_jobs growth-only)** TERBUKA + terikat F13: `:46-47` hanya tumbuh; fix F13 sekaligus menambah TTL/eviction.
- **F10 (OR-gate approval)** TERBUKA + anotasi: kini di `:915` (pass-24 menyebut `:904` — pergeseran baris) dan `:563`; vektor eskalasi praktis = F11, bukan OR-gate.
- **F2/F8** TERBUKA, di luar area F, tanpa bukti baru.

**Audit rute (tugas 3)**: `/api/webhook` dan `/api/hasil/:token` punya rl_wrap 20/menit dan 30/menit (`router_full.cpp:315-316`) sesuai desain; rute API publik lain plain BY DESIGN — gating in-handler lengkap; D7 tidak di-re-report; tidak ada rute plain-unguarded lain.

**Kualitas remediasi pass-24 di area F**: F1 (P33) intinya BAIK — `respond_channel` protobuf-aware (`webhook.cpp:88-139`), fail-closed DB-down (`fail_closed_db` → 503/protobuf success=false; PG-down TIDAK membuka bypass), cek expiry → disable OTP + reject, bump attempts atomik RETURNING (`auth_store.cpp:334-357`, lockout n≥5), lookup per-user `:174`, normalize_phone. SATU-SATUNYA cacat = F12 (bug refactor baru dalam remediasi itu sendiri). F5: disiplin release baik di mana-mana KECUALI F12. F4: `is_safe_version` `utils.cpp:145-152` terverifikasi baik.

**Terverifikasi-baik (12, file:baris)**: persist_submission_pending `exams.cpp:192-215` (referensi pg_used); exam_pdf 4-lapis `:845-870`; device_approved fail-closed `:400-422`; exam_result anti-TOU binding `:1102-1113` (stored_job/stored_exam/stored_mac + fingerprint identitas — mencegah baca lintas-tenant via job_id tebak); exam_by_token PG-first `:684-719`; rate-limit SEBELUM token-check `:557-563` + advisory-lock cap `:579-597` (max 500 default); higien RealPool `pool_real.cpp:63-68`/`:85-91`/`:44-45` (connect_timeout=5 P34-G3, koneksi baru di luar mutex); `auth_store.cpp:291-332` (referensi fix F12); is_safe_version; privasi hasil `hasil.cpp:518`/`:588-592` + escape_like `:547`; pass persist worker `submission_queue.cpp:432-479` (dasar Diretrak F17); webhook fail-closed `:88-139` + normalize_phone.

Batas lingkup agen F: tidak me-re-report G15/sub-G8, try_acquire_job, G17, D7, daftar-closed; 1 FAIL baseline diterima sebagai artefak TDD; tidak menulis file; aturan .env dipatuhi.

### 5.4 Area G — queue/store/db/redis/utils/server-core *(laporan agen G — MASUK)*

**0 HIGH** — build break `main.cpp:170` sudah live-fixed dan **bentuknya BENAR** (diverifikasi agen G + build terisolasi hijau): satu `global_pool()` proses-wide untuk bootstrap+jobs+handlers (`main.cpp:46-56`, ping :85 lewat pool yang sama); `DbPool(...).sanitized_url()` :89 hanyalah display-stub ter-mask, bukan pool kedua. Temuan baru: **8 MEDIUM (G11–G18), 1 LOW (G19, dead code), 2 LOW re-report (G5, G7), 12 catatan LOW/caveat.** Paling berdampak operasional: G14 (job loss senyap), G15 (Redis restart = worker mati seumur proses), G17 (health selalu "ok"), G18 (churn koneksi per request system-apps).

#### MEDIUM

**G11 — MEDIUM — `RealPool::connect()` tanpa `connect_timeout`** — `src/db/pool_real.cpp:14-21` (`PQconnectdb` telanjang) vs `new_connection()` `:45-46` yang benar menambah `connect_timeout=5`. Boot (`main.cpp:47`) memanggil `connect()` → PG blackhole (firewall DROP, bukan RST) = hang tanpa batas → server menggantung saat startup tanpa pesan, container never-ready. Fix: `connect()` panggil `new_connection()` / append `connect_timeout=` dengan cara yang sama.

**G12 — MEDIUM — `acquire()` tanpa cap in-flight, tanpa health-check conn idle, pola double-hold** — `pool_real.cpp:50-61`; `max_conns_` hanya membatasi ukuran `idle_`, BUKAN total koneksi proses. Cold-start/avalanche (8 worker + request handler + 3 timer) = 12+ koneksi fisik baru sekaligus, bisa menembus `max_connections` PG pada burst. Conn idle yang diputus server (restart PG / idle-timeout) TIDAK ditandai — `acquire()` mengembalikan conn mati; kontrak hanya aman karena semua caller saat ini cek `PQstatus` sendiri (`with_global_pg` retry-sekali; caller manual) → kontrak rapuh. Sub: `with_global_pg` + `run_batch` double-hold (wrapper pegang conn #1 selama batch, lambda acquire conn #2) → 2× footprint selama batch besar. Fix: counter in-flight + `std::condition_variable`; `PQstatus`-check + sekali `PQreset`/discard di `acquire()`; dokumentasikan kontrak single-conn per stack.

**G13 — MEDIUM — `pg_conninfo_from_url`: nilai conninfo tanpa quoting/escaping** — `src/db/pool.cpp:74-78` (`user=`/`password=`/`dbname=` tanpa quote/escape). Spasi, `'`, atau `\` di password/user/dbname merusak parsing conninfo (spasi = separator parameter); lebih buruk, password bisa **meng-inject parameter conninfo lain** (mis. ` dbname=postgres` mengubah target DB). Sub: (a) `url_decode_simple` :35 mendekode `+`→spasi di USERINFO — konvensi itu hanya berlaku untuk form-encoded query, bukan userinfo → password literal `pa+ss` berubah jadi `pa ss`; (b) IPv6 rusak — :68 kehadiran `]` me-skip ekstraksi port → host utuh `[::1]:5432` dikirim ke libpq → invalid. **Verified good**: port kosong di-skip (:75); query-string passthrough utuh; `sslmode` eksplisit (`disable`/`verify-full`) DIHORMATI; parameter berulang later-wins; default kini `prefer` (P36-koreksi terkonfirmasi :86-91, :120). Fix: helper `escape_conninfo_value()` (single-quote + escape `\'` `\\`); `+` hanya di-decode di query; strip bracket `[...]` sebelum cari port.

**G14 — MEDIUM — Fallback durability `checked_requeue` menulis ke Redis yang sama yang baru saja gagal** — `src/queue/submission_queue.cpp:325-329` (`push_failed`: hasil `lpush_checked_` DIBUANG `(void)`) + `:344-354`. Skenario yang memicu requeue-gagal adalah **Redis down penuh** → fallback lalu LPUSH ke Redis yang mati itu; `store_result` → `set_` juga unchecked → **job hilang TANPA JEJAS LOKAL sama sekali** (failed-queue P21-T5 hanya hidup ketika Redis hidup). Jawaban siswa hilang senyap; guru lihat siswa "belum submit". Fix: spool file lokal (append-only JSONL, replay saat Redis pulih) atau minimal `utils::log_error` + metrik/alarm; jangan `(void)` hasil `lpush_checked_` di jalur kegagalan.

**G15 — MEDIUM — hiredis: tanpa reconnect, tanpa connect-timeout, AUTH tanpa username (re-report G6 diperluas) + sub G6/G8** — `src/redis/redis_real.cpp:42` (`redisConnect` tanpa timeval, tanpa opsi reconnect), `:46` (`AUTH %s` tanpa username — ACL Redis 6+ diabaikan), `:52-55` (hasil `SELECT` diabaikan → `redis://host:6379/2` diam-diam menulis ke db 0). Tidak ada `redisReconnect`/re-init ctx di seluruh codebase; ctx thread_local dibuat sekali seumur proses (`main.cpp:135-137`) → **Redis restart sekali = 8 worker BRPOP mati PERMANEN (antrean submission tidak pernah dikonsumsi lagi sampai restart proses) + presence WS mati**; `RedisClient::ping()` (`src/redis/client.cpp:9-13`, hpp:11) hanya cek scheme URL + return flag `connected` — bukan PING nyata → health tidak pernah mendeteksi. Sub G8 (path dikoreksi — **AKTIF di production**): `enqueue_job_to_redis()` `src/handlers/api/exams.cpp:138-152` — `g_enqueue_hook` HANYA di-set oleh test (`set_submit_enqueue_hook_for_test`, dipanggil eksklusif dari tests/) → di production `g_enqueue_hook==nullptr` → `:1000-1002` SELALU jatuh ke `enqueue_job_to_redis` → **koneksi TCP+AUTH baru pada setiap submit siswa** (fungsional benar: LPUSH dicek → 503; persist-pending fail-closed P18-C3 tetap lebih dulu; tapi churn 1 koneksi per submit di jam sibuk). Sub tambahan: `try_acquire_job` (`redis/client.cpp:16-44`) guard bocor — `:28-29` error `redis_get` → `""` → jatuh ke lock in-process `g_locks` → return true → **dua replika bisa double-run job yang sama** (expiry/approval/retention). Fix: `redisConnectWithOptions` (timeout) + wrapper `redisReconnect()` saat `ctx->err`; `AUTH user pass` (atau HELLO); `SELECT` dicek; enqueue lewat `sq`/`lpush_checked` yang sudah ada (bukan koneksi per-submit); `try_acquire_job` membedakan error vs unlocked.

**G16 — MEDIUM (server-core) — `Server::stop()` memanggil uWS dari thread yang salah** — `src/server/server.cpp:643-647`: `g_app->close()` dari main thread padahal Loop hidup di `g_uWS_thread` (App dibuat+listen+run di thread itu, :417); `g_app` (static `unique_ptr`) ditulis dari thread uWS dan dibaca main **tanpa sinkronisasi** → uWS tidak thread-safe lintas thread = UB/data race; shutdown bisa hang atau crash sesekali (sulit direproduksi). Posix stub path tidak kena. Fix: `g_app->loop()->defer([&]{ g_app->close(); })` lalu join thread; `g_app` lokal thread + pointer atomik/flag stopped.

**G17 — MEDIUM (server-core) — `/api/health` selalu melaporkan sehat (hardcoded)** — `server.cpp:662-664`: `health_json` return `"db":"ok","redis":"ok","queue":0` literal. Monitoring/LB yang memakai `/api/health` tetap lihat `status: ok` saat DB/Redis mati → traffic terus masuk, incident terdeteksi terlambat; juga menyembunyikan dampak G15 dari operator. Fix: health baca `global_pool()->ping()` real + Redis PING real + `LLEN kQueueKey` untuk queue depth; sertakan degraded vs down.

**G18 — MEDIUM — `system_apps_page`: SELURUH jalur return kecuali fall-through tidak release koneksi** *(konfirmasi kandidat koordinator; termasuk verifikasi semantik deleter + accounting)* — `src/handlers/admin/settings.cpp:363-428`: `real.acquire()` :366, lalu SEMUA early-return — 400 (:371), 404 (:372), 500 (:376), 502 (:385), 200-delete (:386), 400 duplikat (:392/:399/:402), 413 (:403), 400 data (:405), 503 storage (:407), 502 (:410), 500 (:412), 201 create (:419), 200 download (:425) — me-return tanpa `real.release(...)`; **satu-satunya** release = fall-through list `:428` (komentar P21-T2 "kembali ke pool proses-wide" hanya benar di jalur itu). Semantik deleter: `PgConnDeleter` = `PQfinish` (`pool_real.hpp:13`) → missing release = koneksi di-CLOSE, bukan dikembalikan ke `idle_`. Accounting: `RealPool` tanpa counter outstanding → dampak = **reuse loss — churn TCP+auth+TLS handshake PG baru per request admin (upload/download/delete/list, sukses maupun error)**, bukan FD leak; diperparah G12 (tanpa cap in-flight) saat burst. Fungsi lain di file yang sama release benar (`load_all_settings` :115 semua path) — ini outlier. Fix: RAII wrapper/scope-guard (dtor: status OK && !pool_full → release, else PQfinish) atau helper `with_global_pg`-style dengan release otomatis.

#### LOW

**G19 — LOW — `execute_transaction_result`: latent use-after-free (SAAT INI DEAD CODE)** — `exam_store_postgres.cpp:243-270`: `:255-257` `auto r=pool_.exec_params(...)` dalam loop → `r` (PgResultPtr) di-destroy tiap akhir iterasi; `:261-262` membaca `PQntuples(last_result)`/`PQgetvalue(last_result,0,0)` SETELAH dtor → dangling → UB. Grep final: **tidak ada satu pun pemanggil** di src/tests/include (hanya deklarasi `exam_store_postgres.hpp:45`) → LOW sekarang, tapi hampir pasti bug HIGH saat pertama kali di-call (INSERT…RETURNING). Fix: `std::optional<PgResultPtr> last;` + `last = std::move(r);`, atau hapus fungsi dead-code.

**G5 — LOW re-report — `Worker::stop()`/queue stop: sleep per-job saat drain** — `submission_queue.cpp:376-393`: final drain memproses backlog dengan sleep per-job → shutdown lambat bila backlog besar. Fix: drain dengan deadline total atau langsung requeue sisa.

**G7 — LOW re-report — `drain_heartbeat_batch`: `BEGIN` tidak dicek** — `submission_queue.cpp:560`: hasil `real.exec_params(conn,"BEGIN",{})` dibuang → conn rusak = seluruh batch heartbeat **di-drop senyap** (presence siswa hilang tanpa jejak). Fix: cek hasil BEGIN seperti `run_batch` (:468-469).

#### Catatan LOW / caveat (12 — tidak dinaikkan jadi temuan)

(1) `drain_heartbeats_once` :620 — `connect_redis` baru per tick 30s → churn ~2 koneksi/menit seumur proses; (2) `RealPool::ping()` :23-33 — memegang `mu_` selama `PQexec` → ping bisa mem-block acquire/release sesaat; (3) signal handler terdaftar TELAT `main.cpp:174-175` (setelah listen/worker start) → SIGTERM saat migrate/hydrate = default kill tanpa drain; (4) start condition sekali-jalan `main.cpp:160/:165/:170` → Redis/DB down SAAT boot = worker/flusher/timer tidak pernah start seumur proses walau lalu pulih (bertautan G15/G17); (5) seed-admin unreachable di build HAS_LIBPQ (:110-115 — syarat `database_url.empty()` tak mungkin di build production; intentional, kebersihan saja); (6) `requeue()` non-checked :302 (jalur tanpa checked-hook; production pakai checked); (7) posix `http_response` reason-phrase "OK" konstan untuk semua status + `Keep-Alive` padahal selalu close (kosmetik stub); (8) `jobs.cpp:60-107` purge mengabaikan hasil DELETE/BEGIN/COMMIT — purge idempoten acceptable, tapi tanpa log bila purge gagal berulang; (9) `reserve_idempotency` :457 area — `std::stoi("")` throw bila completed-row `response_status` NULL; (10) **`pool_global` env-snapshot** (`pool_global.hpp:25-45`) — conninfo dibekukan `std::call_once` + magic static; `DATABASE_URL` berubah setelahnya tak tercermin → **caveat, bukan bug production** (single-env per proses; dampak praktis hanya test yang flip env dalam satu proses); (11) `with_global_pg` :52-66 — `fn` melempar → `release` dilewati → conn ditutup dtor (aman dari leak) tapi tidak kembali ke pool; (12) `next_id` fallback error → 1 (:366-367 area; sequence-sync setval di migrate menjaga konsistensi).

#### Terverifikasi baik (agen G, dengan bukti)

1. **FIFO queue end-to-end** — LPUSH+BRPOP, requeue via LPUSH menempatkan retry di paling belakang (urutan benar), retry bounded 3× (`kMaxRetries`), backoff 250ms×2^retries cap 5s (tidak ada infinite loop), JobResult TTL 3600 + failed-queue P21-T5. Tugas terstruktur 2c sehat — kecuali G14 saat Redis mati penuh.
2. **Global pool tunggal** — `global_pool()` thread-safe (`call_once` + magic static), urutan init benar (main sebelum worker/handler); `~RealPool` mem-`PQfinish` seluruh `idle_` (`pool_real.cpp:10`); tidak ada koneksi bocor di jalur global (kebocoran reuse spesifik = G18, bukan FD leak); P33-G9/P34 fix tidak regresi.
3. **Batch worker integrity** — SAVEPOINT `submission_job` per job + `pg_advisory_xact_lock(hashtext($1)::bigint)` + COMMIT dicek `:468-469` + INSERT ON CONFLICT fallback + backoff max-sekali → no-double-write tetap utuh.
4. **Hub redis serialization** — `redis_mu_` (`hub.hpp:53`) menserialisasi seluruh akses ctx bersama → kandidat data-race sebelumnya RESOLVED.
5. **`utils.cpp` bersih** — `parse_pg_or_iso_utc` (P33-Ef) parser ketat + kabisat + offset; `is_safe_version` (P33-F4) benar; `generate_token` distribusi uniform (modulo 32). Tidak ada temuan.
6. **Konektivitas URL** — port kosong di-skip, sslmode eksplisit dihormati (tidak ditimpa), default `prefer` (P36-koreksi terkonfirmasi), query-string passthrough utuh.
7. **`release()` semantik BENAR** — status≠OK → PQfinish; `idle_` penuh → PQfinish; else kembali ke pool (`pool_real.cpp:63-68`).
8. **Timer jobs.cpp pattern bersih** — `cv_.wait_for(interval, predicate)` + `try{fn()}catch(...)` + stop flag+notify+join; `try_acquire_job` TTL-lock self-healing; semua job pakai `with_global_pg` (P34 fix benar).
9. **Posix path guards baik** — path traversal, frame cap 5MB, MSG_NOSIGNAL; uWS production path benar (App+listen+run di thread sendiri, per-request state `shared_ptr`, cap 5MB, header forwarding lengkap).
10. **`config.cpp` parsing env defensif** + `validate()` fail-closed lengkap (SECRET ≥32, PORT, DATABASE_MAX_CONNS 1..150, scheme URL) — tidak membaca file `.env` secara langsung.

### 5.5 Area I — nginx/compose/Dockerfile

Cakupan agen I: `nginx/nginx.conf`, `docker-compose.yml`, `Dockerfile`, `.dockerignore`, `.env.example`. **1 HIGH + 2 MEDIUM + 7 LOW baru.**

**I11 — HIGH — `.dockerignore` (fix pass-24 I3) kontradiksi dengan COPY Dockerfile → `docker compose build` GAGAL.**
`.dockerignore:10-12` (`.git`/`.gitignore`/`.github`), `:23-26` (`docs`/`scripts`/`.claude`/`*.md`) vs `Dockerfile:10` (`COPY scripts scripts`) dan `Dockerfile:13` (`COPY .gitignore .stylelintrc.json MIGRASI_STATUS.md docs-cutover.md ./`). Path yang di-exclude tak pernah masuk build context → COPY gagal ("not found"; docker tidak membedakan "absen" dari "di-exclude"); baris 10 gagal lebih dulu, baris 13 akan gagal pada tiga sumber. Keduanya dikomit dalam commit `a3d9acd` TANPA pernah memicu build. **Dampak**: stack tidak bisa dibangun — webui-cpp gagal → nginx (depends_on service_healthy) tak pernah start = deployment produksi rusak total. Catatan: `scripts/` sepenuhnya tooling lokal, CMakeLists 0 referensi — COPY scripts non-esensial. **REGRESI remediasi pass-24**: bug ini diintroduksi oleh fix I3 pass-24 sendiri. **Fix**: (1) hapus `Dockerfile:10` dan sumber non-esensial dari `:13`; atau (2) pola negasi `!scripts` dst. **Pin TDD**: parse daftar `<src>` COPY vs pola .dockerignore — tidak boleh ada sumber tereksklusi (pin saat ini `test_p34_tdd.cpp:238-243` hanya grep `.env` — tidak melindungi kelas ini).

**I12 — MEDIUM — fix pass-24 I2 adalah NO-OP: `limit_except POST { allow all; }` tidak membatasi limit ke POST.**
`nginx.conf:87-90`. Semantik nginx: limit_except mendefinisikan aturan akses untuk semua method KECUALI POST; `allow all` memperbolehkan non-POST; `limit_req` (`:90`) pada level location berlaku untuk SEMUA method termasuk GET. Skenario risiko asli tetap hidup: GET /login (satu IP / banyak pengguna NAT) menghabiskan 2r/s+burst=5 → POST legitimate kena 429. Pin `test_p34_tdd.cpp:232-233` hanya grep string — mengunci teks non-fungsional. **Fix**: `map $request_method $login_key { default $binary_remote_addr; GET ""; HEAD ""; }` + `limit_req_zone $login_key zone=login:10m rate=2r/s;` (key kosong tidak dihitung nginx) + hapus limit_except; update pin. Catatan derivatif: regex `~ ^/(login|admin/login)` tanpa end-anchor cocok juga `/login-anything` — saat ini nol rute C++ demikian, dampak nol, disarankan `^/(login|admin/login)(?:[/?]|$)`.

**I13 — MEDIUM — env gap compose: TURNSTILE_*, EXAMVAN_SECRET_PREV, EXAMVAN_TRUST_PROXY, EXAMVAN_CORS_ORIGINS, STORAGE_PATH tidak dioper.**
compose `environment:` `:61-78` (14 var) vs konsumen `config.cpp:31,32,42`, `login.cpp:149-152` (`bool turnstile_enabled = !tsecret.empty();`), `.env.example` (juga tanpa TURNSTILE_*). **Dampak**: Turnstile mati senyap di produksi (fail-open level config); rotasi secret mustahil (ganti EXAMVAN_SECRET = memutus semua sesi admin karena tak ada PREV); operator tanpa sinyal (tidak ada `${TURNSTILE_SECRET:-}`). **Fix**: tambahkan ke environment compose (fail-closed `:?ERR:...` atau default eksplisit), dokumentasikan di .env.example; pin TDD: setiap `std::getenv` di config.cpp muncul di compose atau whitelist terdokumentasi.

**I14–I20 — LOW**:
- **I14** — `/ws/` (`nginx.conf:62-74`) tanpa add_header → hanya mewarisi CSP (`:42`), tanpa nosniff/XFO/Referrer-Policy; perluas pin `test_review_round3_tdd.cpp:74-101` (R3_S08) ke /ws/ + system-apps.
- **I15** — Redis `mem_limit: 64M` (compose:49) tanpa `--maxmemory`/`--maxmemory-policy` (command `:39` hanya appendonly/appendfsync/requirepass) → OOM-kill saat fork rewrite AOF; fix `--maxmemory 48mb --maxmemory-policy noeviction` (fail-closed untuk queue).
- **I16** — float tag tanpa digest pin: postgres:16-alpine (`:3`), redis:7-alpine (`:29`), nginx:1.27-alpine (`:97`), gcc:13-bookworm, debian:bookworm-slim.
- **I17** — `server_tokens off;` belum diset.
- **I18** — service nginx tanpa healthcheck (db/redis/webui-cpp punya); fix busybox wget `wget -qO- http://localhost:80/api/health`.
- **I19** — upstream cpp_backend (`nginx.conf:8`) tanpa `keepalive N;`, `Connection ""` hanya di /api/health (`:100`) → TCP churn per-request; fix keepalive 32 + proxy_http_version 1.1 + Connection "" di non-WS.
- **I20** — STORAGE_PATH config mati (`config.cpp:35` default `/app/storage`, container read_only:true `:86` tanpa volume; 0 konsumen di src/) — laten EROFS; hapus field.

**Re-verify pass-24**: I4 (detail hilang di kompaksi pass-24) — diserap audit baru I11-I20. I5 — catatan sisi-compose: healthcheck webui-cpp `curl -f http://localhost:5000/api/health` (compose:88-93, parameter benar) selalu hijau saat PG/Redis down (handler statis = G17, closed sisi-server, tidak dihitung lagi); gate depends_on service_healthy untuk nginx (compose:108-109) hanya mengukur liveness proses; aksi compose = pindah ke probe dependency-readiness setelah G17 difix.

**Diretrak pass-24 I6–I10 (semua dengan bukti)**: (a) SSL/HSTS off = disengaja, terdokumentasi (`nginx.conf:32-33,37-38` komentar certbot/Cloudflare; `:40-41` HSTS memang tidak di port 80 sesuai P23-B5 dengan komentar RFC 6797 benar; compose:105 nginx loopback-only 127.0.0.1:8081); (b) go_backend down = scaffolding terdokumentasi (:4-6; tidak ada rute di map $uri $backend :13-20 ke go_backend); (c) map-all→cpp = control point cutover terdokumentasi (:10-12); (d) sisa gap security-header = I14 saja.

**Kualitas remediasi pass-24 di area I**: I1 carve-out body-size OK memadai & konsisten (`nginx.conf:113` 105m hanya /admin/api/system-apps; paritas router `router_full.cpp:55`; jalur upload PDF ujian = multipart POST /admin/api/exams dengan limit app MAX_PDF=5MB cocok default nginx 5m `:34`, tanpa carve-out; satu-satunya jalur 100MB = APK system-apps; margin 5MB disengaja). I2 non-fungsional (→I12). I3 intennya benar (`.env` tereksklusi `.dockerignore:5-7`, tanpa COPY .env*) tapi scripts/.gitignore/*.md bertabrakan COPY (→I11).

**Terverifikasi-baik (11)**: (1) isolasi jaringan — db/redis tanpa ports, network internal; webui-cpp hanya expose 5000; nginx loopback; (2) env fail-closed `${EXAMVAN_SECRET?ERR:...}`/`${DB_PASSWORD?ERR:...}`/`${REDIS_PASSWORD?ERR:...}`/`${R2_ACCESS_KEY_ID:?ERR:...}`/`${R2_SECRET_ACCESS_KEY:?ERR:...}`; (3) .env tereksklusi build context + tanpa COPY .env*; (4) fix G4 benar (compose DATABASE_URL tanpa sslmode + default prefer kompatibel postgres:16-alpine ssl=off); (5) hardening non-root (postgres 70:70, redis 999:999, nginx 101:101, webui-cpp USER examvan uid 10001 Dockerfile:34) + read_only + init:true + no-new-privileges + tmpfs; (6) security headers 4 location (`nginx.conf:55-58`/`:78-81`/`:104-107`/`:121-124`) dengan `always`; (7) volumes + redis appendonly everysec; (8) parameter healthcheck lengkap (redis `:44-48` AUTH ping memvalidasi password saat runtime); (9) konsistensi port end-to-end (5000/5432/6379/80↔8081) nol konflik; (10) konsistensi path nginx↔router C++ (semua location privileged punya rute riil); (11) zone rate-limit admin g_admin_rl 100/60s tetap benar, tanpa interferensi zone login.

**Cek konsistensi (tugas 4): KONSISTEN** — semua 14 nama env compose terverasi terbaca (kontras: yang hilang persis I13; tidak ada nama env compose orphan).

Batas lingkup agen I: aturan .env dipatuhi (nilai kredensial tidak pernah dicetak); tidak me-re-report temuan server-side (G17 dsb.) kecuali konsekuensi sisi-compose.

### 5.6 Area H — frontend JS
*(menyusul — agen H, wave-2; area dipotong di pass-24)*

### 5.7 Area J — admin templates
*(menyusul — agen J, wave-2; area dipotong di pass-24)*

### 5.8 Area K — public templates
*(menyusul — agen K, wave-2; area dipotong di pass-24)*

### Kandidat dalam verifikasi — disposisi final (semua sudah masuk §5)
- ~~`settings.cpp:152-160` protobuf hardcoded + `:144-150` tipe pesan VoucherList~~ → **E-t (§5.2, MEDIUM, VALID)**.
- ~~`settings.cpp:366-428` release-discipline `system_apps_page`~~ — **KONFIRMASI (agen G) → dilapor sebagai G18 (§5.4)**: semua jalur return kecuali fall-through :428 tidak release; deleter `PgConnPtr` = `PQfinish` → koneksi di-close, bukan dikembalikan; RealPool tanpa counter outstanding → dampak churn koneksi per request admin, bukan FD leak. Agen E memperluas kelas yang sama ke `submissions.cpp:212-217`/`:296-301` (item pass-24 **E-d**, masih terbuka — satu kelas remediasi tunggal dengan G18).
- ~~`settings.cpp:421-426/435` listing fake-empty~~ → **instance E-q (§5.2, MEDIUM, VALID)** — keluarga read fake-empty luas lintas file admin.
- ~~`settings.cpp:410` verify gagal tanpa remove → orphan R2~~ → **E-u (§5.2, MEDIUM, VALID)**.

## 6. Daftar Closed / Tidak Di-lapor Ulang

**Pass-20..23 (closed oleh commit sebelumnya):**
- T1–T9 — closed `aea5e8f`
- H7, M7, M8, L3 — closed `060e176`
- M16, M19, B3, B4, B5, B6 — closed `390fd0b`
- M9 (pola lama fake-empty), C5, B1 — closed `1ff5b06`
- P32 heartbeat-flusher (pin `tests/test_p32_tdd.cpp`)

**Remediasi pass-24 — commit `a3d9acd` + `a5046f1`, SEMUA pin hijau (847 PASS @03:20 mencakup seluruh pin P33/P34/P35):**
- Batch-1 HIGH (P33): E-a (users.cpp admin-write fail-closed), E-f ×2 (voucher expiry parse), E-g (update_settings false-200), E-h, D2, I1 (nginx body-size), F1 ×3 (webhook OTP), F4 ×2 (X-Version)
- Batch-2 MEDIUM (P34): G2 (checked_requeue), G1, G9, E-c, F5, D5, AuthStore-global-pool, G3, G4 (+koreksi `a5046f1`), D1, F7, I2, I3
- Batch-2b (P35): D3a, D3b, D4a, D4b, D4c

**Catatan**: pin hijau ≠ bebas bug BARU hasil refactor — memburu bug baru adalah tugas utama pass-25.

## 7. Aturan Dedup

Keluarga stack-pool (pool koneksi stack-lokal lintas file: `admin/exams.cpp`, `pengawas.cpp`, `export.cpp`, `hasil.cpp`, `api/exams.cpp`, `router_full.cpp`, `auth_store.cpp`, `jobs.cpp`, `main.cpp`, `submission_queue.cpp`) = **SATU kelas temuan** — sudah diremediasi menjadi `db::global_pool()` / `with_global_pg()` dan dipin hijau (G1/G9/E-c/F5/D5/AuthStore). Tidak di-lapor ulang per-file.
