# Status Migrasi Go → C++20 (examvan-opt)

Ikuti `migrasi-cpp/00..05` + `docs-cutover.md`. Ringkasan verifikasi:

## Gerbang F0–F8

| Fase | Gerbang | Bukti |
|---|---|---|
| F0 profiling | target RSS ≤100MB/10k WS, p99 ≤500ms | `scripts/load_test/k6_ws.js` (1k→10k) + `soak_check.sh` |
| F1 kontrak | 40+ route + WS protokol beku | `scripts/contract.json` (extract Go), `parity_harness.py` |
| F2 skeleton | build+sanitizer+Docker+PG/Redis pool hijau | `76 tests` release+ASan, `Dockerfile` multi-stage, `docker-compose.yml` |
| F3 WS hub | auth cookie, join room, heartbeat, backpressure | `hub.cpp` port 594b Go, `test_hub` 9 cases, uWS adapter `server/` |
| F4 public | /hasil, /download SSR html-structural parity | `handlers/public` + `characterization` |
| F5 sesi | HMAC dual-key, CSRF, Turnstile, 426 version gate | `session/cookie` dual, `test_new_coverage` |
| F6 admin | CRUD user/voucher/exam + R2 SigV4 presign | `handlers/admin/*` + `handlers/r2` |
| F7 jobs/XLSX | queue LPush/BRPop retry3 batch50 + SETNX job lock + XLSX PK | `queue/` `export` `jobs` |
| F8 dual-run | nginx map per-grup + shadow diff + 7-hari hijau | `nginx.conf` map, `shadow_proxy.py`, `docs-cutover.md` |

## Paritas

- Endpoint JSON → json-schema (field & tipe sama)
- SSR → html-structural (selector & teks kunci)
- Cookie `examvan_session` HMAC-SHA256 dual-key → sesi tidak terputus saat switch
- Skema PG tidak berubah (schema.sql Go referensi, repo baru tidak bikin volume prod)
- APK/desktop kontrak beku (tidak paksa update)

## Verifikasi Harian

```bash
cmake -B build && cmake --build build -j && ./build/examvan-tests  # 76 PASSED
./build-san/examvan-tests  # ASan+UBSan hijau
for f in static/js/*.test.mjs; do node --test "$f"; done  # 68 guard ALL PASSED
k6 run --env WS_URL=ws://localhost:8081/ws/1 scripts/load_test/k6_ws.js
python3 scripts/shadow_proxy.py --go http://go:5000 --cpp http://cpp:5000
```

## Rollback

`nginx.conf` edit satu baris `map` → `go_backend` + `nginx -s reload` (<1m). PG/Redis/R2 single source, zero drift.
