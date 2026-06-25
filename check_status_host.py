#!/usr/bin/python3
import json
with open('/home/abharcable/HUNTER_status.json') as f:
    d = json.load(f)
db = d.get("db", {})
pp = d.get("provisioned_ports", [])
bal = d.get("balancers", [])
print(f"Phase: {d.get('phase')} Paused: {d.get('paused')}")
print(f"DB total={db.get('total')} alive={db.get('alive')}")
print(f"Provisioned ports: {len(pp)}")
for p in pp[:5]:
    print(f"  port={p['port']} alive={p['alive']} engine={p['engine_used']}")
print(f"Balancers: {len(bal)}")
for b in bal:
    print(f"  port={b['port']} type={b['type']} running={b['running']} backends={b['backends']} healthy={b['healthy']}")
print(f"upstream_tunnels: {len(d.get('upstream_tunnels', []))}")
print(f"generated_configs: {len(d.get('generated_configs', []))}")
