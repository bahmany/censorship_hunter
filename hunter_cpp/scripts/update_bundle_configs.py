#!/usr/bin/env python3
import base64
import json
import re
import socket
import sys
import urllib.parse
import urllib.request
from pathlib import Path

SCHEME_RE = re.compile(r"(vmess|vless|trojan|ss)://[^\s\"'<>]+", re.IGNORECASE)

DEFAULT_SOURCES = [
    "https://raw.githubusercontent.com/barry-far/V2ray-config/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt",
    "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/vless",
    "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/reality",
]


def mirrors(url: str):
    out = [url]
    raw = "https://raw.githubusercontent.com/"
    if not url.startswith(raw):
        return out
    rest = url[len(raw):]
    parts = rest.split("/", 3)
    if len(parts) != 4:
        return out
    owner, repo, branch, path = parts
    gh = f"{owner}/{repo}@{branch}/{path}"
    out.extend([
        f"https://cdn.jsdelivr.net/gh/{gh}",
        f"https://fastly.jsdelivr.net/gh/{gh}",
        f"https://gcore.jsdelivr.net/gh/{gh}",
    ])
    return out


def extract_endpoint(uri: str):
    u = uri.strip()
    if u.startswith("vmess://"):
        raw = u[len("vmess://"):].split("#", 1)[0]
        pad = (-len(raw)) % 4
        raw += "=" * pad
        try:
            obj = json.loads(base64.b64decode(raw).decode("utf-8", "ignore"))
            return f"{obj.get('add','')}:{obj.get('port','')}"
        except Exception:
            return ""
    if u.startswith("ss://"):
        host = u.split("@")[-1]
        host = host.split("#", 1)[0].split("?", 1)[0]
        return host
    p = urllib.parse.urlsplit(u)
    return f"{p.hostname or ''}:{p.port or ''}"


def endpoint_key_by_ip(endpoint: str):
    host, sep, port = endpoint.partition(":")
    if not host or not sep:
        return ""
    try:
        ip = socket.gethostbyname(host)
    except Exception:
        ip = host
    return f"{ip}:{port}"


def load_sources(runtime_sources: Path):
    if not runtime_sources.exists():
        return DEFAULT_SOURCES
    urls = []
    for line in runtime_sources.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) >= 10 and cols[0] == "1":
            url = cols[9].strip()
            if url.startswith("http://") or url.startswith("https://"):
                urls.append(url)
    return urls or DEFAULT_SOURCES


def fetch(url: str):
    req = urllib.request.Request(url, headers={"User-Agent": "huntercensor-bundler/1.0"})
    with urllib.request.urlopen(req, timeout=15) as r:
        return r.read().decode("utf-8", "ignore")


def main():
    root = Path(__file__).resolve().parents[2]
    runtime = root / "runtime"
    config = root / "config"
    runtime.mkdir(exist_ok=True)
    config.mkdir(exist_ok=True)

    sources = load_sources(runtime / "sources_manager.tsv")
    raw_uris = []
    for src in sources:
        body = ""
        for u in mirrors(src):
            try:
                body = fetch(u)
                if body:
                    break
            except Exception:
                continue
        if body:
            raw_uris.extend([m.group(0) for m in SCHEME_RE.finditer(body)])

    seen_endpoint = set()
    seen_ip = set()
    unique = []
    for uri in raw_uris:
        endpoint = extract_endpoint(uri)
        if not endpoint:
            continue
        key_ip = endpoint_key_by_ip(endpoint)
        if endpoint in seen_endpoint:
            continue
        if key_ip and key_ip in seen_ip:
            continue
        seen_endpoint.add(endpoint)
        if key_ip:
            seen_ip.add(key_ip)
        unique.append(uri.strip())

    out_text = "\n".join(unique) + ("\n" if unique else "")
    (config / "All_Configs_Sub.txt").write_text(out_text, encoding="utf-8")
    (config / "all_extracted_configs.txt").write_text(out_text, encoding="utf-8")
    (config / "sub.txt").write_text(out_text, encoding="utf-8")
    (runtime / "HUNTER_config_db_export.txt").write_text(out_text, encoding="utf-8")
    print(f"[bundle] sources={len(sources)} configs={len(unique)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
