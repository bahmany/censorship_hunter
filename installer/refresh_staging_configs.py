#!/usr/bin/env python3
import re
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEED_CPP = ROOT / "hunter_cpp" / "src" / "core" / "seed_data.cpp"

URI_RE = re.compile(r"(?:vmess|vless|trojan|ssr?|hysteria2|hy2|tuic)://[^\s\"'<>]+", re.IGNORECASE)


def load_seed_sources(limit: int = 20):
    text = SEED_CPP.read_text(encoding="utf-8", errors="ignore")
    urls = re.findall(r"https?://[^\"\s]+", text)
    out = []
    for u in urls:
        if "raw.githubusercontent.com" in u and u not in out:
            out.append(u)
        if len(out) >= limit:
            break
    return out


def fetch(url: str, timeout: int = 15) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "hunter-setup-refresh/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read().decode("utf-8", errors="ignore")


def extract_uris(blob: str):
    return URI_RE.findall(blob)


def write_lines(path: Path, lines):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        for line in lines:
            f.write(line + "\n")


def main():
    sources = load_seed_sources(20)
    if not sources:
        print("[refresh] no seed sources found", file=sys.stderr)
        return 1

    print(f"[refresh] downloading from {len(sources)} sources (ordered)")
    all_uris = []
    seen = set()

    for i, url in enumerate(sources, start=1):
        try:
            body = fetch(url)
            uris = extract_uris(body)
            added = 0
            for uri in uris:
                if uri not in seen:
                    seen.add(uri)
                    all_uris.append(uri)
                    added += 1
            print(f"[refresh] {i:02d}/{len(sources)} ok +{added} {url}")
        except Exception as exc:
            print(f"[refresh] {i:02d}/{len(sources)} fail {url} -> {exc}")
        time.sleep(0.15)

    if not all_uris:
        print("[refresh] no configs fetched", file=sys.stderr)
        return 2

    targets = [ROOT / "installer" / "staging", ROOT / "release_package"]
    for base in targets:
        write_lines(base / "config" / "sub.txt", all_uris)
        write_lines(base / "config" / "All_Configs_Sub.txt", all_uris)
        write_lines(base / "config" / "all_extracted_configs.txt", all_uris)
        write_lines(base / "runtime" / "HUNTER_config_db_export.txt", all_uris)
        write_lines(base / "runtime" / "seed_configs.txt", all_uris)

    print(f"[refresh] wrote {len(all_uris)} unique configs into installer/release staging")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
