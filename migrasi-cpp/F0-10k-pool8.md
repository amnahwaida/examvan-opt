# F0 — 10k WS Pool 8 + SOMAXCONN (2026-08-25 13:00)

**Build**: `71904f2` POSIX `pool 8 + SOMAXCONN` (bukan `WITH_UWEBSOCKETS`), `webui-cpp:5000` via `k6` 10k/2m.

## Hasil vs Sebelum (thread-per-conn, backlog 128)

| Metrik | Sebelum (thread-per-conn) | Pool 8 + SOMAXCONN | Target |
|---|---|---|---|
| `ws_sessions` | 37030 (24680 pong) | **28160** (24189 pong) | — |
| `connected` | 66% (24680/37029) | **85%** (24189/28159) | — |
| `pong` | 79.98% | **92.41%** | — |
| `ws_connecting` p90/p95/p99 | 30.99s / 60s / 60s | **34.78s / 40.4s / 60s** | <500ms |
| `iteration p99` | 60s | **60s** | <5000ms |
| `RSS` | 6.66 MiB | **6.66 MiB** (sama) | ≤100 MB/10k |

**Analisis**: Pool 8 + SOMAXCONN → `connected` 66→85% (+19pp), `p95` 60s→40s, tapi `p99` tetap 60s (k6 timeout 60s). 8 thread untuk WS 5s long-lived → max 8 konkuren, 10k antri → p99 tetap. **Butuh uWS event-loop 1 thread** (`App().ws` per-socket) untuk `p99 <500ms`. RAM tetap `PASS` 0.6 KB/conn.
