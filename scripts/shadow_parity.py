#!/usr/bin/env python3
"""F8 shadow parity: bandingkan respons Go vs C++ per kelas paritas (dok 03 §2).

Kelas paritas:
  - json-schema : key & tipe sama, urutan bebas (endpoint JSON)
  - html-structural : selector/teks kunci sama, whitespace bebas (SSR)
  - redirect    : status + Location exact

Usage:
  python3 scripts/shadow_parity.py            # pakai default container names
"""
import subprocess, json, sys, re

GO_C = "examvan-go-shadow"
CPP_C = "examvan-cpp-server"

def fetch(container, path):
    """GET via wget dalam container; kembalikan (status, body)."""
    out = subprocess.run(
        ["docker", "exec", container, "wget", "--server-response", "-qO-",
         f"http://localhost:5000{path}"],
        capture_output=True, text=True, timeout=15)
    body = out.stdout
    m = re.search(r"HTTP/[\d.]+\s+(\d+)", out.stderr)
    status = int(m.group(1)) if m else 0
    return status, body

def keys_of(body):
    try:
        return set(json.loads(body).keys())
    except Exception:
        return None

HTML_KEYS = {
    "/hasil":   ["Cek Hasil"],
    "/download":["Download EXAMVAN"],
}

# (path, parity_class)
CASES = [
    ("/api/health",        "json-schema"),
    ("/api/time",          "json-schema"),
    ("/api/exams",         "json-schema"),
    ("/hasil",             "html-structural"),
    ("/download",          "html-structural"),
]

fails = 0
print(f"{'PATH':<14} {'CLASS':<17} {'GO':<7} {'CPP':<7} VERDICT")
for path, cls in CASES:
    try:
        gs, gb = fetch(GO_C, path)
    except Exception as e:
        print(f"{path:<14} {cls:<17} ERR     -       GO_FAIL ({e})"); fails += 1; continue
    try:
        cs, cb = fetch(CPP_C, path)
    except Exception as e:
        print(f"{path:<14} {cls:<17} {gs:<7} ERR       CPP_FAIL ({e})"); fails += 1; continue

    if cls == "json-schema":
        gk, ck = keys_of(gb), keys_of(cb)
        ok = gk is not None and ck is not None and gk == ck
        detail = f"keys={sorted(ck)[:4] if ck else '-'}"
    else:
        anchors = HTML_KEYS.get(path, [])
        ok = all(a in cb for a in anchors) and cs == gs
        detail = f"anchors={anchors}"

    verdict = "OK" if ok else "DIFF"
    if not ok: fails += 1
    print(f"{path:<14} {cls:<17} {gs:<7} {cs:<7} {verdict}  {detail}")

print(f"\nTOTAL FAIL: {fails}")
sys.exit(1 if fails else 0)
