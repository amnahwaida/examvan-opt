# Arsitektur EXAMVAN-Opt — C++20 + uWebSockets + Protobuf Mandatory untuk 2-Core 8GB

> Dokumen terpisah — arsitektur yang ditegakkan untuk mengejar performa optimal di perangkat STB 2-core / 8GB (target: 500 perangkat × 1.000 ujian, ratusan ribu koneksi WS serentak).
> Status: **MANDATORY** — JSON didepresiasi, Protobuf menjadi format wire wajib (fail-closed `415` bila JSON dikirim saat `PROTOBUF_MANDATORY=1`).

---

## 1. Mengapa C++ + uWebSockets di 2C/8GB

### 1.1 Dinding Kapasitas Terukur

Profiling `EXAMVAN` Go (`roadmap_kapasitas.md`, `F0`) menunjukkan dinding bukan CPU/DB melainkan **RAM per koneksi WebSocket**:

| Implementasi | RAM / koneksi | 10k koneksi | 100k koneksi | Catatan |
|---|---|---|---|---|
| Go `gorilla/websocket` | 50–150 KB | 0.5–1.5 GB | 5–15 GB (OOM) | `read goroutine` + `write goroutine` + `channel` per client |
| C++ `uWebSockets` `uSockets 0.8.8` + `libusockets` | **1–3 KB** | **10–30 MB** | **100–300 MB** | `per-socket userdata` tanpa thread per koneksi, `epoll`/`kqueue` single loop |

Pada STB **2-core / 7-8 GB** (1 GB untuk OS + DB cache, 512 MB untuk `webui-cpp` hard limit `docker-compose.yml:61`), 100k koneksi WS Go habis di `~5 GB` sebelum DB sempat menjawab; C++ muat di `~300 MB` dengan sisa untuk `PG pool 60` + `Redis` + `R2 presign`.

### 1.2 Model Eksekusi

```
nginx (map $request_uri $backend) per-grup
  ├─ go_backend: down (F8)  ─┐
  └─ cpp_backend:5000 ───────┼─► PG 16 (150 conn, 60 pool, 256M shared_buffers)
                             ├─► Redis 7 (AOF everysec, heartbeat 5m TTL)
                             └─► R2 SigV4 (presign, upload libcurl)
```

- **Event loop tunggal** `uWS::App::run()` di thread terpisah (`server.cpp:337` `g_uWS_thread`) — tidak `thread per koneksi`.
- **POSIX fallback** `Server::listen` 8→16 worker `queue<fd> + condition_variable` untuk parity tanpa `WITH_UWEBSOCKETS` (CI, builder).
- **Hub** `websocket/hub.cpp` `rooms_: unordered_map<string, set<shared_ptr<Client>>>` snapshot + fan-out tanpa lock di hot path; `Client::try_send` backpressure `256` `try_send+unregister` paritas Go `594 baris`.
- **Sesi** `examvan_session` HMAC-SHA256 dual-key (`EXAMVAN_SECRET` + `PREV`) rotasi tanpa logout, `b64` constant-time `diff|=`.

### 1.3 Anggaran Memori 2C/8GB (STB)

| Komponen | Limit | Realita p50 |
|---|---|---|
| `webui-cpp` RSS | 512 MB hard (`mem_limit`) | 180–250 MB @ 50k WS |
| `db` | 1 GB + 256M `shared_buffers` | 600 MB |
| `redis` | 64 MB | 12 MB |
| `nginx` | 64 MB | 8 MB |
| OS + buffers | ~1 GB | — |
| **Total** | **~2.6 GB** | **sisa 5 GB untuk burst** |

Targets `F0`: `p99 connecting <500 ms`, `RSS/1k conn <30 MB`, `soak 24h` datar (`soak_check.sh`).

---

## 2. Mengapa JSON → Protobuf Mandatory

### 2.1 Biaya JSON di Hot Path

Hot path terpanas: `heartbeat` `1/menit × 500 device × 1.000 ujian = 500k msg/menit`, `exam_completed`, `ListExams` page 10, `submit` batch 50.

Profil `utils/sanitize.cpp` + `hub.cpp:85` `extract_json_string` manual `string::find` → alokasi `std::string` per field, `json_string` escape `"` `\` `\b\f\n\r\t` + `\uXXXX` per `<0x20`, `marshaling` `marshal_socketio` `["event",payload]` string copy 2×, `helpers::parse_form` `url_decode` + `map` insert per field.

Pada 500k msg/menit, JSON menambah:
- **30–50 % byte** dibanding varint binary (field name diulang tiap pesan: `"student_name"`, `"mac_address"` 13–15 byte × 6 field = ~80 byte overhead / pesan)
- **2–3× CPU** parsing (branch `in_str`, `esc`, `find('"')` per byte) + alokasi `std::map` + `std::string` per field
- **GC pressure** di JS frontend (`JSON.parse` per `apiFetch` + `api:error` normalizer)

Di STB 2-core, ini menjadi penjepit: 2 core × 3 GHz ~ 6 Gops, JSON parsing 500k × ~2 µs = 1 detik CPU penuh per menit hanya untuk `heartbeat` — sebelum `scoring`, `R2`, `DB`.

### 2.2 Keuntungan Protobuf Mandatory

| Aspek | JSON | Protobuf `proto3` |
|---|---|---|
| Ukuran wire | `{"student_name":"Budi",...}` ~120 B | `Heartbeat{student_name:"Budi"}` ~45 B (varint + tag 1 byte) |
| Parse | O(n) scan + alloc per field | `ParseFromArray` zero-copy, `arena` reuse |
| Schema | implisit (field typo = silent) | `.proto` kontrak beku, `required` optional explicit |
| Validasi | manual `find` + `sanitize_ws_field` | `protoc` generated `has_field()` + `sanitize` tetap |
| Versi | string compare `compare_versions` | `required`/`optional` + reserved tags |
| CPU | `extract_json_string` per field `O(n*m)` | tag dispatch `switch(field_number)` `O(n)` |

Target: **-40 % byte WS**, **-60 % CPU hot path**, **-30 % p99**, **-25 % RSS** pada `k6_ws.js` 10k→50k.

### 2.3 Mandatory, Bukan Opsional

`PROTOBUF_MANDATORY=1` (default `1` di `production`, `0` di `development` untuk migrasi):

- `Content-Type: application/json` → `415 Unsupported Media Type` `{"error_code":"PROTOBUF_REQUIRED","message":"Gunakan application/x-protobuf"}`
- `Accept: application/json` → `406 Not Acceptable` (klien harus `Accept: application/x-protobuf` atau `application/vnd.examvan.v1+protobuf`)
- WS `payload` → binary `uWS::OpCode::BINARY` dengan `Heartbeat`/`ExamCompleted` protobuf, bukan `TEXT` `["event",json]`
- Queue `examvan:submissions:pending` `LPush/BRPop` → `bytes` protobuf, bukan `to_json()` string

Dual-support dipertahankan **hanya** selama jendela migrasi `APP_ENV=development` + `PROTOBUF_MANDATORY=0` (log `WARN` per JSON request).

---

## 3. Skema Protobuf (Kontrak Beku)

Lokasi: `proto/examvan.proto` (dipetakan dari `migrasi-cpp/03` + `models/*.hpp` + `queue/submission_queue.hpp`).

```proto
syntax = "proto3";
package examvan.v1;
option cc_enable_arenas = true;

// Heartbeat pengawas — menggantikan {"student_name":...,"mac_address":...} JSON
message Heartbeat {
  int32  exam_id       = 1;
  string mac_address   = 2; // sanitize_ws_mac, cap 100
  string student_name  = 3; // cap 200, strip &<>"'`=
  string exam_number   = 4; // cap 100
  string student_class = 5; // cap 100
  string device_info   = 6; // cap 200
  string last_seen     = 7; // RFC3339, server generate
  string event         = 8; // "heartbeat"
}

// Selesai ujian
message ExamCompleted {
  int32  exam_id     = 1;
  string mac_address = 2;
  string event       = 3; // "exam_completed"
}

// Envelope WS — menggantikan Socket.IO ["event",payload_json]
message WsEnvelope {
  string event   = 1; // "heartbeat" | "exam_completed" | "pong" | "student_update"
  bytes  payload = 2; // Heartbeat / ExamCompleted serialized
}

// Ujian (create/list)
message Exam {
  int32  id           = 1;
  string name         = 2; // 1-255, sanitize
  string token        = 3; // 8 A-Z0-9
  string file_path    = 4; // R2 key exams/<id>/<filename>
  int64  size_bytes   = 5;
  string status       = 6; // "active" | "inactive"
  string created_at  = 7; // RFC3339
}
message CreateExamRequest {
  string name         = 1;
  string file_path    = 2;
  int64  size_bytes   = 3;
  string custom_token = 4; // optional, 8 A-Z0-9
  bytes  pdf_data     = 5; // multipart file bytes, %PDF magic wajib
}
message CreateExamResponse {
  bool   success = 1;
  int32  id      = 2;
  string token   = 3;
  string name    = 4;
  string file_path = 5;
}

// Queue
message SubmissionJob {
  string job_id        = 1;
  int32  exam_id       = 2;
  string student_name  = 3;
  string mac_address   = 4;
  int32  retries       = 5;
  string enqueued_at  = 6;
}
```

`protoc --cpp_out=src/proto --grpc_out=...` menghasilkan `examvan.pb.h/.cc` (`cc_enable_arenas` untuk reuse arena di hot path `Hub::handle_heartbeat`).

---

## 4. Perubahan Build & Runtime

### 4.1 Dependencies

`vcpkg.json`:
```json
"dependencies": ["openssl","libpq","hiredis","gtest","libcurl","zlib","protobuf","abseil"]
```

`CMakeLists.txt`:
```cmake
find_package(Protobuf REQUIRED)
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS proto/examvan.proto)
target_link_libraries(examvan_core PUBLIC protobuf::libprotobuf)
```

`Dockerfile` builder: `apt-get install -y protobuf-compiler libprotobuf-dev`

### 4.2 Middleware Mandatory

`src/middleware/protobuf.cpp` (baru):
- `is_protobuf_content(req) -> bool` cek `Content-Type: application/x-protobuf`
- `require_protobuf(req) -> optional<Response>` jika `Config::protobuf_mandatory` && `!is_protobuf` → `415` / `406`
- `parse_protobuf<T>(req.body, &msg)` via `ParseFromArray` + arena

`src/config/config.hpp`:
- `bool protobuf_mandatory = !is_development()` (env `PROTOBUF_MANDATORY` override)

### 4.3 Migrasi Bertahap

| Fase | `APP_ENV` | `PROTOBUF_MANDATORY` | Perilaku |
|---|---|---|---|
| **M0 dual** (minggu 1) | `development` | `0` | Terima JSON + Protobuf, log `WARN` per JSON, metric `protobuf_fallback_total` |
| **M1 shadow** | `production` | `0` | Terima keduanya, `k6_ws.js` banding `p99 json vs protobuf` |
| **M2 mandatory** | `production` | `1` | Tolak JSON `415`, WS `TEXT` → `CLOSE 1003`, queue `JSON` → `DLQ` |

---

## 5. Performa 2C/8GB — Target & Validasi

| Metrik | Sebelum (JSON) | Target Protobuf Mandatory |
|---|---|---|
| `RSS/1k WS` | 28 MB | **18 MB** (-35 %) |
| `p50 heartbeat` | 18 ms | **7 ms** |
| `p99 heartbeat` | 420 ms | **160 ms** |
| `10k WS connect burst` | 44 s | **19 s** |
| `exam creation 5M PDF` | 1.8 s (JSON + base64) | **0.9 s** (bytes) |
| `queue LPUSH` | 120 B JSON | **45 B** protobuf |

Validasi: `k6 run scripts/load_test/k6_ws.js --vus 500 --duration 10m` + `soak_check.sh 24h` RSS datar + `scripts/parity_harness.py --go ... --cpp ... --protobuf` 0-diff.

---

## 6. Perintah Operasional

```bash
# Build dengan protobuf
cmake -B build -DWITH_UWEBSOCKETS=ON -DWITH_PROTOBUF=ON
cmake --build build -j$(nproc)
./build/examvan-tests --gtest_filter=Protobuf.*

# Jalankan mandatory di produksi
PROTOBUF_MANDATORY=1 docker compose up -d --build
curl -X POST http://localhost:8081/admin/api/exams \
  -H "Content-Type: application/json" # → 415 PROTOBUF_REQUIRED
curl -X POST http://localhost:8081/admin/api/exams \
  -H "Content-Type: application/x-protobuf" --data-binary @req.pb # → 201

# Rollback (darurat, <1 menit)
# nginx: map $request_uri tetap cpp_backend (tidak ada go_backend lagi F8)
# app: set PROTOBUF_MANDATORY=0 && docker compose up -d --build
```

---

## 7. Referensi

- `src/websocket/hub.cpp:48` `broadcast_to_room(event,payload_json)` → akan menjadi `payload bytes`
- `src/helpers/utils.cpp:41` `generate_token` → tetap, token di dalam `Exam.token`
- `src/queue/submission_queue.cpp:19` `to_json()` → `SubmissionJob.SerializeToString`
- `migrasi-cpp/03` kontrak beku → `.proto` adalah turunan biner dari kontrak tersebut
- `F0` profiling `scripts/load_test/k6_ws.js` → baseline sebelum/ sesudah protobuf
