#!/usr/bin/env bash
# test_p31_env.sh — kontrak lingkungan pass-23 (TDD RED untuk M16/B4/B5/B3/M19).
#
# Kontrak infra yang tidak bisa diuji lewat gtest C++ (docker-compose,
# nginx.conf, script shell) dicek di sini — dipanggil CI sebagai guard
# regresi konfigurasi deployment.
#
# Usage: bash tests/test_p31_env.sh   (exit 0 = semua kontrak lulus)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0
ok(){ echo "  PASS: $1"; }
bad(){ echo "  FAIL: $1"; fail=1; }

echo "== M16: docker-compose hardening (redis/db/nginx) =="
COMPOSE="$ROOT/docker-compose.yml"
grep -q -- '--requirepass' "$COMPOSE" \
  && ok "redis dijalankan dengan --requirepass" \
  || bad "redis TANPA --requirepass (M16: siapa pun di network bisa baca antrean submission)"
grep -q 'REDIS_PASSWORD' "$COMPOSE" \
  && ok "REDIS_URL webui-cpp memakai password dari env" \
  || bad "REDIS_URL masih tanpa password"
grep -Eq '^[[:space:]]*read_only:[[:space:]]*true' "$COMPOSE" \
  && [ "$(grep -c 'read_only: true' "$COMPOSE")" -ge 4 ] \
  && ok "read_only: true >= 4 layanan (db/redis/webui-cpp/nginx)" \
  || bad "read_only: true belum diterapkan ke semua layanan (>=4)"
[ "$(grep -c '^[[:space:]]*user:' "$COMPOSE")" -ge 3 ] \
  && ok "user: non-root untuk db/redis/nginx" \
  || bad "user: non-root belum ada untuk db/redis/nginx"

echo "== B4: nginx — location / tidak lagi meneruskan header Upgrade =="
NGINX="$ROOT/nginx/nginx.conf"
awk '/location \/ \{/,/^    \}/' "$NGINX" | grep -q 'proxy_set_header Upgrade' \
  && bad 'location / masih set header Upgrade/Connection upgrade (B4: WS di luar /ws/ putus 60s)' \
  || ok "location / bersih dari header Upgrade"
awk '/location \/ \{/,/^    \}/' "$NGINX" | grep -q 'proxy_read_timeout 130s' \
  && ok "location / punya proxy_read_timeout 130s (fallback WS)" \
  || bad "location / tidak punya fallback proxy_read_timeout 130s"

echo "== B5: nginx — HSTS tidak dikirim di port 80 (HTTP) =="
grep -n 'Strict-Transport-Security' "$NGINX" | grep -v '^\s*#' | grep -q . \
  && bad "HSTS masih dikirim padahal TLS belum aktif (B5: header diabaikan browser di HTTP, menyesatkan audit)" \
  || ok "HSTS dihapus sampai TLS diaktifkan (placeholder listen 443 tetap)"

echo "== B3: check-docker-paths.sh paralel =="
CHK="$ROOT/scripts/check-docker-paths.sh"
grep -q 'xargs' "$CHK" \
  && ok "check-docker-paths.sh memakai xargs (paralel -P)" \
  || bad "check-docker-paths.sh masih loop serial (B3: CI >8 menit, rawan timeout)"
grep -q 'JOBS' "$CHK" \
  && ok "jumlah job paralel bisa diatur (JOBS)" \
  || bad "tidak ada variabel JOBS untuk paralelisme"

echo "== M19: extract_contract.py tanpa path absolut =="
EX="$ROOT/scripts/extract_contract.py"
grep -q '/home/vannyezha' "$EX" \
  && bad "extract_contract.py masih hardcode path absolut /home/vannyezha (M19: mati di CI/mesin lain)" \
  || ok "extract_contract.py bebas path absolut"
grep -q -- '--src' "$EX" && grep -q -- '--out' "$EX" \
  && ok "extract_contract.py menerima --src/--out" \
  || bad "extract_contract.py tidak menerima --src/--out"
python3 "$EX" --src /nonexistent/main.go >/dev/null 2>&1
[ $? -ne 0 ] && ok "exit code != 0 saat file sumber tidak ada" \
  || bad "exit code 0 saat file sumber tidak ada (gagal senyap)"

echo
if [ "$fail" -eq 0 ]; then
  echo "OK: semua kontrak pass-23 lulus."
else
  echo "GAGAL: kontrak pass-23 belum terpenuhi (lihat FAIL di atas)." >&2
fi
exit "$fail"
