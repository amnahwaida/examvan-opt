# F0 — Profiling & Baseline (Gerbang G0)

Tanggal: 2026-08-25 · Pelaksana: vannyezha · Metode: TEST-FIRST (harness F0 ditulis sebelum ukur)

## 1. Target F0 (harus tertulis sebelum ukur, dok 00 §5)

| Metrik | Target | Alasan |
|---|---|---|
| RSS per 10k WS | ≤100 MB | roadmap_kapasitas: 1000 ujian ×500 dev = 500k koneksi butuh t620 7.2 GB |
| p99 WS connecting | <500 ms | UX monitoring real-time |
| p99 submit deadline | <800 ms | burst submit serentak |

## 2. Baseline Terukur

### Go (EXAMVAN `webui-server` 2.7.3, Gin+gorilla/websocket 594b)

| Kondisi | RSS | Keterangan |
|---|---|---|
| idle (0 conn) | **36.89 MiB** | `docker stats --no-stream` |
| setelah 2791 WS attempt 100 VU/35s (tanpa auth) | **43.45 MiB** (+6.5 MiB) | k6 `grafana/k6` `WS_URL=ws://examvan-go-f0:5000/ws/1` — semua `connected` **GAGAL 100%** karena Go butuh sesi/token (auth guard `02 §10`), jadi bukan koneksi valid — RSS naik hanya untuk HTTP 302/401, bukan WS nyata |
| p99 `ws_connecting` (gagal) | 31.35 ms (p99) — tidak valid karena gagal |  |

> Go idle 37 MiB = baseline tinggi sebelum 1 koneksi. Literatur gorilla/websocket ~40–60 KB/conn → 10k ≈ 400–600 MB (tanpa room overhead).

### C++ (examvan-opt `webui-cpp` POSIX+Hub)

| Kondisi | RSS |
|---|---|
| idle (0 conn) | **4.77 MiB** |
| setelah same 2791 attempt | **5.26 MiB** (+0.5 MiB) — sama: WS upgrade stub `JSON 101` bukan handshake RFC6455, jadi k6 juga gagal `connected 0%`, tapi hub logic `try_send 256` + sanitize tetap diuji 76 tests hijau |

### Simpulan F0 Sementara

- **C++ 7× lebih irit baseline** (4.8 vs 36.9 MiB) sebelum koneksi — sinyal kuat untuk WS layer.
- **Belum dapat angka per-koneksi valid** karena kedua server butuh auth cookie untuk WS 101 dan C++ belum handshake RFC6455 penuh (server POSIX `handle_client` hanya `Router::dispatch` → `JSON 101`, bukan `Sec-WebSocket-Accept`).
- p99 submit belum diukur (butuh `wrk/k6` HTTP `POST /api/exams/:id/submit` 202 queue).

## 3. Gap & Rekomendasi

| Gap | Mitigasi F3 |
|---|---|
| WS perlu `Cookie: examvan_session` valid HMAC dual-key (dok 03 §4) | `session/cookie` sudah dual-key + `b64url` — tambah `k6` login `POST /login` ambil `Set-Cookie` lalu `ws.connect` dengan `headers:{Cookie}` |
| C++ perlu handshake RFC6455 + `per-socket userdata` uWS | `server/server.cpp` ganti `handle_client` → `uWS::App::ws` `WITH_UWEBSOCKETS=ON` (FetchContent `uSockets v0.8.8` + `uWebSockets v20.71.0` sudah siap di CMake) |

## 4. Keputusan G0

**G0 = Opsi B (hybrid sidecar) Lanjut** — alasan:

1. Dinding `RAM/WS` terbukti via baseline 7× + literatur 10–50× (dok 00 §2.1) — di sinilah C++ unggul.
2. Dinding `CPU+Postgres` tidak terbukti perlu C++ (F0 belum ukur submit p99, tapi roadmap sudah: indeks+pool+hardware, bukan bahasa).
3. Risiko F3 terkecil (hanya Hub), rollback `nginx map` <1m, 76 tests + 68 guard hijau.

**Opsi C (full rewrite 4–6 bulan) DITUNDA**sampai Opsi B F3+F8 live 7 hari dan bukti p99 non-WS masih di atas target.

## 5. Next F0 Lanjut (sebelum masuk F1)

- [ ] Implement `server` handshake RFC6455 (atau aktifkan `WITH_UWEBSOCKETS`) + `k6` login flow → ukur ulang `RSS/1k conn` dan `p99 connecting <500 ms` valid.
- [ ] `wrk`/`k6` HTTP `POST /api/exams/:id/submit` deadline burst → p99 submit.
- [ ] Simpan `docker stats` + `k6 --out json` sebagai artefak CI.

