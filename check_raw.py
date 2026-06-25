#!/usr/bin/python3
import json
d = json.load(open('/home/abharcable/HUNTER_status.json'))
pp = d.get('provisioned_ports', [])
print(f"Total provisioned: {len(pp)}")
for p in pp[:5]:
    print(f"  port={p['port']} engine={p['engine_used']!r} alive={p['alive']} uri={p['uri'][:50]}")
# Check balancer pool
bal = d.get('balancers', [])
for b in bal:
    print(f"  balancer: port={b['port']} type={b['type']} running={b['running']} backends={b['backends']} healthy={b['healthy']}")
