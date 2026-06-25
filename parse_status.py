#!/usr/bin/python3
import json, sys
d = json.load(sys.stdin)
pp = d.get('provisioned_ports', [])
print(f"{len(pp)} ports")
for p in pp[:3]:
    print(f"  port={p.get('port')} engine={p.get('engine_used','')!r} alive={p.get('alive')} uri={p.get('uri','')[:40]}")
