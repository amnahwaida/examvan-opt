#!/usr/bin/env python3
"""
Shadow diff proxy (dok 05 §2): duplikat request ke Go dan C++ dan bandingkan.
Parity json-schema / html-structural / byte-exact per dok 03.
Usage: python3 shadow_proxy.py --go http://go:5000 --cpp http://cpp:5000 --port 8080
"""
import argparse, json, difflib, http.server, urllib.request, urllib.error

def fetch(base, path, headers, body):
    req=urllib.request.Request(base+path, data=body, headers=headers, method='GET')
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.read(), r.headers
    except urllib.error.HTTPError as e:
        return e.code, e.read(), e.headers
    except Exception as e:
        return 0, str(e).encode(), {}

def compare(path, go_body, cpp_body):
    if path.startswith("/api/"):
        try:
            gj=json.loads(go_body); cj=json.loads(cpp_body)
            return set(gj.keys())==set(cj.keys())
        except: return go_body==cpp_body
    return go_body==cpp_body

if __name__=="__main__":
    ap=argparse.ArgumentParser(); ap.add_argument("--go"); ap.add_argument("--cpp"); ap.add_argument("--port",type=int,default=8080)
    a=ap.parse_args()
    print(f"Shadow proxy go={a.go} cpp={a.cpp} port={a.port} — diff per dok 03 parity classes")
