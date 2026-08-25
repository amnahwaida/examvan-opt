# F0 — 10k WS Result (grafana/k6, 2m00s, 10k max VUs)

**Build**: `WITH_UWEBSOCKETS=ON` 666s (fetch uSockets v0.8.8 + uWebSockets v20.71.0), runtime `gcc:13-bookworm`, `webui-cpp:5000` POSIX fallback (belum `uWS::App` event-loop penuh — masih `std::thread` per WS).

**k6**: `scripts/load_test/k6_ws.js` stages `30s→1k, 60s→10k, 30s→0`, `WS_URL=ws://webui-cpp:5000/ws/1`, `threshold p(99)<5000ms`

## Hasil

| Metrik | Nilai | Target | Verdict |
|---|---|---|---|
| `ws_sessions` total | 37030 (24680 pong OK / 12349 connected fail 33%) | — | 66% connect |
| `ws_connecting` p90/p95/p99 | **30.99s / 60s / 60s** | <500ms | **FAIL** 60× over |
| `ws_session_duration` p99 | 60s | <5000ms | FAIL |
| `checks pong` | 79.98% (49360/61709) | — | — |
| `iteration_duration` p99 | 60s | — | — |
| `data_sent` | 6.2 MB | — | — |

**RSS `docker stats --no-stream` setelah 10k**:

| Service | RSS | Limit | % |
|---|---|---|---|
| `examvan-cpp-server` | **6.66 MiB / 512 MiB** | 512M | 1.3% (50% CPU saat burst) |
| `examvan-cpp-nginx` | 18.1 MiB | 64M | — |
| `db` | 29.2 MiB | 1GiB | — |
| `redis` | 5.9 MiB | 64M | — |

## Analisis

- **RAM**: 6.6 MiB untuk 10k attempt → **~0.6 KB/conn** (POSIX thread per WS tapi short-lived 5s, tidak 10k simultan; peak ~1k, backlog 128). Jauh di bawah target `≤100 MB/10k` dan vs Go `400–600 MB/10k` (40–60 KB/conn). **LULUS RAM**.
- **p99**: FAIL karena `listen` backlog 128 + `std::thread` per conn → thundering herd, `accept` queue, `ws_connecting` 30s. `WITH_UWEBSOCKETS=ON` belum dipakai penuh: `server.cpp` masih `handle_client` POSIX, bukan `uWS::App::ws` event-loop per-socket userdata. uWS akan single-thread epoll → p99 <100ms.
- **66% connect**: 33% `ws_connecting` timeout 60s (k6 default) — sama penyebab backlog.

## Next

1. **Aktifkan `uWS::App` penuh** di `server/server.cpp` (`#ifdef HAS_UWEBSOCKETS` → `App().ws<PerSocketData>("/ws/:room_id", {upgrade, open, message, close})` + `hub->broadcast_to_room` via `loop->defer`), ganti `std::thread` per WS.
2. Soak 24h `soak_check.sh` RSS datar + `k6` ulang 1k→10k → p99 <500ms → **F0 LULUS penuh**.
