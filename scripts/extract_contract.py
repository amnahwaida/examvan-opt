#!/usr/bin/env python3
"""F1: extract Gin routes from main.go into frozen contract JSON (dok 03 §2)."""
import re, json, pathlib
main = pathlib.Path("/home/vannyezha/project/sekolah/EXAMVAN/webui/cmd/server/main.go").read_text()
routes = []
for m in re.finditer(r'r\.(GET|POST|PUT|DELETE|PATCH)\("([^"]+)"', main):
    routes.append({"method": m.group(1), "path": m.group(2), "auth": "unknown"})
for m in re.finditer(r'\.Group\("([^"]+)"', main):
    pass
out = pathlib.Path(__file__).parent / "contract.json"
out.write_text(json.dumps(routes, indent=2))
print(f"Wrote {len(routes)} routes to {out}")
