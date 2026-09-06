#!/usr/bin/env bash
# check-docker-paths.sh — compile semua sumber examvan_core dengan flag yang
# SETARA dengan Docker build (-Werror + HAS_LIBPQ/HAS_HIREDIS/HAS_LIBCURL)
# memakai stub header di scripts/stubs, TANPA perlu libpq-dev/hiredis-dev/
# libcurl4-openssl-dev terpasang lokal.
#
# Tujuan: menangkap error yang hanya muncul di Docker build (jalur #ifdef
# HAS_* yang dilompati compiler lokal tanpa dev headers) sebelum menunggu
# build container ~9 menit — mis. simbol dipakai sebelum dideklarasikan,
# -Werror=misleading-indentation, salah namespace.
#
# Pakai:
#   scripts/check-docker-paths.sh            # cek semua sumber di CMakeLists.txt
#   scripts/check-docker-paths.sh src/foo.cpp # cek file tertentu
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STUBS="$ROOT/scripts/stubs"
SRC_ROOT="$ROOT/src"
CURL_STUB="$STUBS/curl"
HIREDIS_STUB="$STUBS/hiredis"
# libpq-fe.h hidup di <prefix>/libpq-fe.h saat include <libpq-fe.h>
PQ_STUB="$STUBS"

CXX="${CXX:-g++}"
STD="${CXX_STD:--std=c++20}"
# Flag setara target_compile_options examvan_core + add_compile_options global
# (lihat CMakeLists.txt): -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter.
FLAGS=(-fsyntax-only "$STD" -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter
       -I"$SRC_ROOT" -I"$ROOT/include"
       -isystem "$PQ_STUB" -isystem "$CURL_STUB" -isystem "$HIREDIS_STUB"
       -DHAS_LIBPQ=1 -DHAS_HIREDIS=1 -DHAS_LIBCURL=1)

# Kumpulkan sumber examvan_core dari CMakeLists.txt (bukan test — test dibuild
# dengan EXAMVAN_TESTING=1 dan lingkungan gtest tersendiri).
mapfile -t SOURCES < <(grep -oE 'src/[A-Za-z0-9_/]+\.cpp' "$ROOT/CMakeLists.txt" | sort -u)
if [ "$#" -gt 0 ]; then SOURCES=("$@"); fi

fail=0
for f in "${SOURCES[@]}"; do
  path="$ROOT/$f"
  [ -f "$path" ] || { echo "MISSING: $f"; fail=1; continue; }
  if ! out=$("$CXX" "${FLAGS[@]}" "$path" 2>&1); then
    echo "FAIL: $f"
    echo "$out" | sed 's/^/    /'
    fail=1
  fi
done

if [ "$fail" -eq 0 ]; then
  echo "OK: ${#SOURCES[@]} file(s) clean dengan flag Docker (HAS_LIBPQ+HAS_HIREDIS+HAS_LIBCURL, -Werror)."
else
  echo "GAGAL: perbaiki error di atas (akan mematahkan docker compose build)." >&2
fi
exit "$fail"
