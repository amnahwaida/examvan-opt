# F8 — Baseline Live (Shadow 10 VUs/5s, 2026-08-25)

> Dijalankan `docker run --network examvan-opt_internal grafana/k6` 10 VUs/5s, `WS_URL` `ws://<host>:5000/ws/1`, `docker stats --no-stream`, `parity harness` via `docker exec wget`.

## 1. Paritas Go vs C++ (snapshot)

| Path | Go (bench 2.7.3) | C++ (POSIX) | Verdict |
|---|---|---|---|
| `/api/health` | 158b `{"status":"healthy",...}` | 33b `{"status":"ok","version":"2.7.2"}` | OK (json-schema: `status` ada) |
| `/api/time` | 70b `{"server_time":"..."}` | 30b `{"now":"..."}` | DIFF key (`server_time` vs `now`) — minor, perlu samakan key `server_time` di C++ untuk `byte-exact` |
| `/hasil` | 5046b `Cek Hasil` html | 86b `Cek Hasil` html-structural | OK (selector `Cek Hasil` sama) |
| `/download` | 59888b `Download EXAMVAN` | 51b `Download EXAMVAN` | OK (paritas `html-structural` via header, body stub) |

> Semua JSON `json-schema` (key `status`/`success`), SSR `html-structural` (teks kunci sama) → **0 diff kritis** (DIFF `/api/time` hanya nama key, bukan tipe).

## 2. WS Load 10 VUs/5s (k6)

| Target | Go `ws_connecting` p90/p95 | C++ `ws_connecting` p90/p95 | Status |
|---|---|---|---|
| `WS_URL` `/ws/1` 10 VUs | **36.3 / 36.6 ms** | **97.5 / 97.8 ms** | ✅ <500 ms target F0 |
| `ws_sessions` 50 | 9.8/s | 9.2/s | — |

- C++ ~2.6× lebih lambat p90 karena **POSIX handshake** (`handle_ws` SHA1 + `send` per-frame) bukan `uWS` per-socket userdata (F3 stub). Masih di bawah target, tapi prod harus `WITH_UWEBSOCKETS=ON` untuk 10k.

## 3. RSS (docker stats --no-stream)

| Service | Go `examvan-go-f0` | C++ `examvan-cpp-server` | Ratio |
|---|---|---|---|
| idle → after 10 VUs | 36.9 → **34.2 MiB** (-2.7) | 4.8 → **5.4 MiB** (+0.6) | **6.3× lebih irit** |
| `db` | 54.76 MiB | 54.76 MiB (shared) | — |
| `redis` | 4.77 MiB | 4.77 MiB (shared) | — |

> Go turun RSS karena GC/shrink setelah burst; C++ naik tipis (+0.6) — stabil, belum leak. Target F0 `≤100 MB/10k` masih jauh.

## 4. Next

- [ ] Samakan `GET /api/time` key `server_time` di C++ → `byte-exact` parity.
- [ ] Aktifkan `WITH_UWEBSOCKETS=ON` + `k6` 1k→10k (30s+60s+30s) ukur p99 + `soak_check.sh` 24h.
- [ ] `parity_harness` cron 5m + `shadow_proxy.py` live 7 hari → `F8-gate` LULUS.
