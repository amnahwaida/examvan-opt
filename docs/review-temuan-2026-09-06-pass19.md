# Review Ulang — Pass-19 (2026-09-06, lanjutan remediasi Pass-18)

Baseline: commit `fix(review-pass18)` (Batch 0-7 TDD), full suite **764 passed + 1 skip**
(`P7_Frontend.JsGuardCount`), tanpa regresi. Metode: verifikasi build+tests,
inspeksi `server.cpp`/`hub.cpp`, grep pola temuan Pass-18.

## Yang terverifikasi FIXED Pass-18

- C1 session `exp/iat` + reject expired; C2 logout+logout_page clear mirror Secure.
- C3 INSERT-before-LPUSH `ON CONFLICT DO NOTHING`; C4 `set_lpush_checked` prod + idempotency.
- C5 main SIGTERM/SIGINT drain (worker/flusher/jobs); uWS thread joinable.
- C6 `static_path_safe` di uWS; H1 rate-limit local fallback; H2 turnstile empty→403.
- H4 `EXAMVAN_TRUST_PROXY` gate + validasi; H5 `redis_mu_`; H6 SETNX-first.
- H8 backpressure `getBufferedAmount`; H9 delegate validasi pengawas; H10 audit INSERT.
- H12 proto Settings tak set password; H13 batch `COMMAND_OK||TUPLES_OK`.
- H14 `MSG_NOSIGNAL` + partial-write; M4 header device/auth/UA/referer; M5 limit 5MB seragam.
- M11 `pending()` locked + single backoff; M12 pool mutex/bound/`SELECT 1` ping + dtor.
- M13 migrate 5 tabel inti; M14 parse `AUTH`/`SELECT db` + no-throw; M17 `env_int` warn.
- M15 map `$uri` + `/ws/` 130s + proto/host headers; M18 SRI; L1 `ESCAPE`; L4 export `LIMIT 50000`.
- P17-C4 revoke approval, C5 UNIQUE+`ON CONFLICT`, C7 identity required, M2 `FOR UPDATE`,
  M3/M4 `pb_gate` student POST + complete, M7 `logs`+`data`, C1-C3 gate/limit.

## Sisa OPEN (prioritas berikutnya)

1. **C5-parsial — POSIX worker masih `detach()`** (`server.cpp:595,604`): graceful
   shutdown hanya join uWS thread; path posix (non-uWS build) tak bisa drain/join.
   Perlu vektor thread joinable + `stop()` join.
2. **H7 — heartbeat/exam_completed tolak non-privileged** (`hub.cpp:152,209`):
   student direct heartbeat selalu drop; perlu keputusan paritas Go
   (relay pengawas vs direct-student) + test WS end-to-end.
3. **M7 — submission/detail tanpa cek in-handler** (100% scope router): split
   PG/memory berisiko 403 salah / bypass; pindahkan cek ke handler + test matrix.
4. **M8 — `edit_user` mass-assignment / bulk scope** (`users.cpp`, `router_full`):
   hardening `roles_json` superadmin + bulk per `exam_pengawas`/operator-instansi.
5. **M9-sisa / M10-sisa** — pola fallback success no-op masih ada di
   `submissions.cpp`, `users.cpp`, `get_auto_approve`; audit assignment best-effort.
6. **P17-M5/M6** — operator lintas instansi (`pengawas_exams` branch `is_priv`);
   result binding tanpa `identity_hash` di fallback (`exam_result`).
7. **M16/M19/L2/L3/L6** — non-kode: docker user/read_only/requirepass, CI uWS/
   protobuf/curl + `nginx -t`/scan, contract drift (`extract_contract.py`,
   `parity_harness.py`), mask length, `copyCode`/`EXAM_TOKEN` escape, versi hardcode.

## Rekomendasi pengujian

1. `docker stop` saat COMMIT/BRPOP/batch pending (posix + uWS) — pastikan nol job hilang.
2. Submit saat Redis down / DB down setelah dequeue / FK violation.
3. Dual-instance token rotation + approval revoke + result identity fingerprint.
4. Admin matrix penuh + export 50k + voucher batch + system-app 100MB + audit tab.
5. Browser login→dashboard→submissions→logout form; WS heartbeat student vs pengawas.
