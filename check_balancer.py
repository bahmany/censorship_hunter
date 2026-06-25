#!/usr/bin/python3
import json

with open('/home/abharcable/HUNTER_balancer_cache.json') as f:
    d = json.load(f)

configs = d.get("configs", [])
alive = [c for c in configs if c.get("alive")]
print(f"Total cached: {len(configs)}, Alive: {len(alive)}")
for c in alive[:5]:
    uri = c["uri"][:60]
    eng = c.get("engine_used", "?")
    lat = c.get("latency_ms", 0)
    print(f"  {uri} engine={eng} latency={lat}")
