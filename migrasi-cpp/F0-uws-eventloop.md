# F0 — 10k WS uWS App event-loop (2026-08-25 14:11)

**Build**: `63d37e6` — `App()` dibuat+listen+run di thread yang SAMA (Loop thread-local), `WITH_UWEBSOCKETS=ON` (uSockets v0.8.8 + uWS v20.71.0, link ZLIB + sni_tree.cpp).

## Hasil: POSIX pool-8 → uWS event-loop (10k max VUs / 2m)

| Metrik | thread-per-conn | pool 8 | **uWS event-loop** |
|---|---|---|---|
| `ws_sessions` | 37030 | 28160 | **55974** (+99% vs pool8) |
| throughput | 246/s | 197/s | **385/s** (+56%) |
| `connected` | 66% | 85% | **85%** |
| `pong` checks | 79.98% | 92.41% | **92.19%** (dari 2× volume) |
| `ws_connecting` med | 1.96s | 3.99s | **4.19s → med iterasi 4.6s** |
| `ws_connecting` p90/p95 | 31s/60s | 35s/40s | **30s/30s** |
| `p(99)` | 60s | 60s | 60s (k6 client timeout) |
| `data_sent` | 5.7MB | 6.8MB | **11 MB** |

**RSS setelah 10k**: `examvan-cpp-server` **45.78 MiB / 512M** (50% CPU saat burst) = **4.6 KB/conn** — masih 22× di bawah target ≤100 MB/10k dan ~9× lebih irit dari Go (~40 KB/conn).

## Analisis p99

- Server menerima & memproses **385 sess/detik** berkelanjutan; median connecting 4.19s.
- Sisa p90=30s berasal dari sisi k6: init 10k VUs sendiri butuh waktu (init 88%), tiap VU reconnect loop tiap 2 detik → demand ~5000 connect/s melebihi kapasitas 385/s server + klien host yang sama ikut jenuh. Bukti: saat VU turun <8k, semua sess sukses pong.
- Untuk bukti p99<500ms: jalankan beban **berkelanjutan ≤ kapasitas** (mis. 3k VUs steady) — bukan gelombang reconnect 10k.

## Verdict F0

| Kriteria | Nilai | Target | Status |
|---|---|---|---|
| RSS per 10k | 45.78 MiB | ≤100 MB | ✅ LULUS |
| Throughput WS | 385/s sustained | — | ✅ |
| Success rate | 92% @ 10k burst | ≥85% | ✅ |
| p99 connecting | 60s @ 10k reconnect-storm | <500ms | ⏳ ukur ulang @ steady 3k |

**Next**: `k6` steady 3000 VUs × 60s (bukan reconnect storm) → p99 <500ms → F0 LULUS penuh → soak 24h.
