#!/usr/bin/python3
import json
d = json.load(open('/home/abharcable/HUNTER_status.json'))
pp = d.get('provisioned_ports', [])
print(f"Total provisioned: {len(pp)}")
alive_count = sum(1 for p in pp if p.get('alive'))
print(f"Alive: {alive_count}")
for p in pp[:5]:
    print(f"  port={p['port']} engine={p.get('engine_used','?')!r} alive={p.get('alive')} uri={p.get('uri','')[:50]}")
bal = d.get('balancers', [])
for b in bal:
    print(f"  balancer: port={b['port']} type={b['type']} running={b['running']} backends={b['backends']} healthy={b['healthy']}")
# Check upstream and generated
print(f"upstream_tunnels: {len(d.get('upstream_tunnels', []))}")
print(f"generated_configs: {len(d.get('generated_configs', []))}")
# DB stats
db = d.get('db', {})
print(f"DB total={db.get('total')} alive={db.get('alive')}")
