#!/usr/bin/env python3
"""
Lapis 2 parity harness (dok 04 §2): compare Go vs C++ responses.
Usage: python parity_harness.py --go http://localhost:5000 --cpp http://localhost:8081
"""
import argparse, json, sys, urllib.request
PATHS = ["/api/health","/","/hasil","/download"]
def fetch(base, path):
    try:
        with urllib.request.urlopen(base+path, timeout=5) as r:
            return r.status, r.read().decode()
    except Exception as e:
        return 0, str(e)
def main():
    ap=argparse.ArgumentParser(); ap.add_argument("--go", required=True); ap.add_argument("--cpp", required=True)
    a=ap.parse_args()
    diffs=0
    for p in PATHS:
        gs,gb=fetch(a.go,p); cs,cb=fetch(a.cpp,p)
        ok = gs==cs
        print(f"{p}: Go={gs} Cpp={cs} {'OK' if ok else 'DIFF'}")
        if not ok: diffs+=1
    sys.exit(1 if diffs else 0)
if __name__=="__main__": main()
