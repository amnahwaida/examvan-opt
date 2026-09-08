#!/usr/bin/env python3
"""F1: extract Gin routes from main.go into frozen contract JSON (dok 03 §2).

P23-M19: hardcode path absolut home-direktori dihapus — script kini menerima
--src (default ./cmd/server/main.go) dan --out (default contract.json di
folder script), sehingga jalan di CI/mesin mana pun. File sumber hilang =
exit 2 dengan pesan jelas (bukan gagal senyap).
"""
import argparse
import json
import pathlib
import re
import sys

ROUTES_RE = re.compile(r'r\.(GET|POST|PUT|DELETE|PATCH)\("([^"]+)"')


def extract(src_text: str):
    routes = []
    for m in ROUTES_RE.finditer(src_text):
        routes.append({"method": m.group(1), "path": m.group(2), "auth": "unknown"})
    return routes


def main() -> int:
    here = pathlib.Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Extract Gin routes from main.go into contract JSON.")
    ap.add_argument("--src", default=str(here.parent / "cmd" / "server" / "main.go"),
                    help="path ke cmd/server/main.go (repo Go)")
    ap.add_argument("--out", default=str(here / "contract.json"),
                    help="path output contract JSON")
    args = ap.parse_args()

    src = pathlib.Path(args.src)
    if not src.is_file():
        print(f"ERROR: file sumber tidak ditemukan: {src}", file=sys.stderr)
        print("Gunakan: python3 scripts/extract_contract.py --src <path>/cmd/server/main.go",
              file=sys.stderr)
        return 2

    routes = extract(src.read_text())
    out = pathlib.Path(args.out)
    out.write_text(json.dumps(routes, indent=2))
    print(f"Wrote {len(routes)} routes to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
