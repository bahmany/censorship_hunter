#!/usr/bin/python3
import json

with open('/tmp/HUNTER_status.json') as f:
    d = json.load(f)

db = d.get("db", {})
pp = d.get("provisioned_ports", [])
bal = d.get("balancers", [])

print(f"Phase: {d.get('phase')} Paused: {d.get('paused')}")
print(f"DB total={db.get('total')} alive={db.get('alive')} tested={db.get('tested_unique')}")
print(f"Provisioned ports: {len(pp)}")
for p in pp[:5]:
    print(f"  port={p['port']} alive={p['alive']} engine={p['engine_used']} uri={p['uri'][:60]}")
print(f"Balancers: {len(bal)}")
for b in bal:
    print(f"  port={b['port']} type={b['type']} running={b['running']} backends={b['backends']} healthy={b['healthy']}")
print(f"Upstream connected: {d.get('upstream_connected')} / {d.get('upstream_total')}")
print(f"Generated active: {d.get('generated_active')} / {d.get('generated_total')}")
ut = d.get("upstream_tunnels", [])
print(f"Upstream tunnels in JSON: {len(ut)}")
for u in ut:
    print(f"  {u.get('name')} connected={u.get('connected')} latency={u.get('latency_ms')}")
gc = d.get("generated_configs", [])
print(f"Generated configs in JSON: {len(gc)}")
for c in gc[:3]:
    print(f"  {c.get('name')} active={c.get('active')} port={c.get('port')}")
