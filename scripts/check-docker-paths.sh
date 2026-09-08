#!/usr/bin/env bash
# check-docker-paths.sh — compile semua sumber examvan_core (dan test) dengan
# flag yang SETARA dengan Docker build (-Werror + HAS_LIBPQ/HAS_HIREDIS/
# HAS_LIBCURL/HAS_UWEBSOCKETS) memakai stub header di scripts/stubs, TANPA
# perlu libpq-dev/hiredis-dev/libcurl4-openssl-dev terpasang lokal.
#
# Tujuan: menangkap error yang hanya muncul di Docker build — jalur #ifdef
# HAS_* yang dilompati compiler lokal tanpa dev headers, termasuk lapisan
# test (mis. blok #ifdef HAS_HIREDIS di tests/ yang butuh include eksplisit).
#
# Pakai:
#   scripts/check-docker-paths.sh              # src (2 pass uWS) + tests (1 pass)
#   scripts/check-docker-paths.sh src/foo.cpp  # cek file tertentu (src 2 pass)
#
# P23-B3: compile paralel via xargs -P — full run 3 pass × ~100 file turun
# dari >8 menit serial ke ~durations/nproc. JOBS mengatur paralelisme
# (default: nproc). Output error tetap dilabeli per file+mode.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STUBS="$ROOT/scripts/stubs"
SRC_ROOT="$ROOT/src"
CURL_STUB="$STUBS/curl"
HIREDIS_STUB="$STUBS/hiredis"
UWS_STUB="$STUBS/uws"
# libpq-fe.h hidup di <prefix>/libpq-fe.h saat include <libpq-fe.h>
PQ_STUB="$STUBS"

CXX="${CXX:-g++}"
STD="${CXX_STD:--std=c++20}"
# Paralelisme: JOBS env override, default semua core. Minimal 1.
JOBS="${JOBS:-$(nproc)}"
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS=1
# Flag setara target_compile_options examvan_core + add_compile_options global
# (lihat CMakeLists.txt): -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter.
BASE_FLAGS=(-fsyntax-only "$STD" -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter
       -I"$SRC_ROOT" -I"$ROOT/include"
       -isystem "$PQ_STUB" -isystem "$CURL_STUB" -isystem "$HIREDIS_STUB"
       -I"$UWS_STUB")

# Lapisan test: gtest dari FetchContent build lokal + protobuf dari sistem
# + examvan.pb.h hasil generate. Bila build lokal belum ada, pass test dilewati
# dengan peringatan (bukan gagal) — jalankan cmake configure dulu.
GTEST_INC=()
PB_FLAGS=()
TEST_SOURCES=()
if [ -d "$ROOT/build/_deps/googletest-src/googletest/include" ]; then
  GTEST_INC=(-isystem "$ROOT/build/_deps/googletest-src/googletest/include"
             -isystem "$ROOT/build/_deps/googletest-src/googlemock/include")
  PB_FLAGS=(-DHAS_PROTOBUF=1 -I"$ROOT/build")
  mapfile -t TEST_SOURCES < <(grep -oE 'tests/[A-Za-z0-9_/]+\.cpp' "$ROOT/CMakeLists.txt" | sort -u)
fi

# Kumpulkan sumber examvan_core dari CMakeLists.txt.
mapfile -t SOURCES < <(grep -oE 'src/[A-Za-z0-9_/]+\.cpp' "$ROOT/CMakeLists.txt" | sort -u)
if [ "$#" -gt 0 ]; then SOURCES=("$@"); fi

fail=0

# compile_one <mode> <file> — satu unit compile -fsyntax-only. Sukses → diam;
# gagal → cetak output berlabel (bukan tulis ke file sampingan).
compile_one(){
  local mode="$1"; shift
  local f="$1"; shift
  local -a extra=("$@")
  local path="$ROOT/$f"
  if [ ! -f "$path" ]; then
    echo "MISSING [$mode]: $f"
    return 1
  fi
  local out
  # Ekspansi defensif (aman utk bash lama dgn set -u saat array kosong).
  if ! out=$("$CXX" ${BASE_FLAGS[@]+"${BASE_FLAGS[@]}"} ${PB_FLAGS[@]+"${PB_FLAGS[@]}"} \
              ${extra[@]+"${extra[@]}"} "$path" 2>&1); then
    echo "FAIL [$mode]: $f"
    echo "$out" | sed 's/^/    /'
    return 1
  fi
  return 0
}

# Mode worker: dipanggil xargs — satu arg NUL-delimited "mode|file|extra".
if [ "${1:-}" = "--worker" ]; then
  shift
  CXX="$1"; shift
  IFS='|' read -r mode file extra_rest <<< "$1"
  extra_rest="${extra_rest:-}"
  compile_one "$mode" "$file" $extra_rest
  exit $?
fi

# Dual pass src: Docker build = uWS ON, build lokal/CI (tanpa -DWITH_UWEBSOCKETS)
# = uWS OFF. Keduanya jalur kompilasi nyata — keduanya wajib bersih.
fail=0
for mode in uws_on uws_off; do
  [ "$mode" = uws_on ] && EXTRA="-DHAS_UWEBSOCKETS=1" || EXTRA="-UHAS_UWEBSOCKETS"
  echo "--- pass: $mode ($EXTRA) ---"
  # Bangun daftar item "mode|file|extra" lalu jalankan paralel.
  items=()
  for f in "${SOURCES[@]}"; do items+=("$mode|$f|$EXTRA -DHAS_LIBPQ=1 -DHAS_HIREDIS=1 -DHAS_LIBCURL=1"); done
  if ! printf '%s\0' "${items[@]}" | xargs -0 -P "$JOBS" -n 1 "$0" --worker "$CXX"; then
    fail=1
  fi
done

# Pass test (Docker parity): HAS_LIBPQ+HAS_HIREDIS+HAS_LIBCURL+HAS_UWEBSOCKETS=1
# + EXAMVAN_TESTING=1, uWS OFF tidak perlu — test tidak menyentuh jalur uWS.
# Catatan: blok HAS_HIREDIS di test hanya aktif di Docker — justru inilah
# yang ingin dicek (include eksplisit, simbol tersedia via stub).
if [ "${#TEST_SOURCES[@]}" -gt 0 ]; then
  echo "--- pass: tests (Docker parity) ---"
  items=()
  for f in "${TEST_SOURCES[@]}"; do
    items+=("tests|$f|-DEXAMVAN_TESTING=1 -DHAS_UWEBSOCKETS=1 -DHAS_LIBPQ=1 -DHAS_HIREDIS=1 -DHAS_LIBCURL=1 ${GTEST_INC[*]} ${PB_FLAGS[*]}")
  done
  if ! printf '%s\0' "${items[@]}" | xargs -0 -P "$JOBS" -n 1 "$0" --worker "$CXX"; then
    fail=1
  fi
else
  echo "SKIP: tests (build/_deps gtest belum ada — jalankan: cmake -B build)"
fi

if [ "$fail" -eq 0 ]; then
  echo "OK: ${#SOURCES[@]} src x 2 pass + ${#TEST_SOURCES[@]} tests bersih (Docker parity, -Werror, paralel -P$JOBS)."
else
  echo "GAGAL: perbaiki error di atas (akan mematahkan docker compose build)." >&2
fi
exit "$fail"
