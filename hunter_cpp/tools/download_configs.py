#!/usr/bin/env python3
"""
download_configs.py — Fetch the latest proxy configs from all public sources
and write a single deduplicated bundle file.

This runs at BUILD TIME (invoked by CMake) so the freshest configs are baked
into the single-file Hunter binary. The same source list is used at runtime
by the C++ ConfigFetcher, but embedding them at build time means the binary
ships with a full working config set on first launch — zero network scraping
required to get started.

Outputs:
  <outfile>  : one URI per line, deduplicated, only lines containing "://"

Usage:
  download_configs.py --outfile <path> [--timeout 30] [--workers 16]
                      [--proxy <url>] [--min-configs 1000]
                      [--sources-file <path>] [--no-base64-decode]

If --sources-file is omitted, the built-in DEFAULT_SOURCES list is used
(which mirrors include/core/constants.h githubRepos()).
"""
import argparse
import base64
import concurrent.futures
import os
import re
import sys
import time
from pathlib import Path
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError

# ─── Default source list — mirrors C++ constants.h githubRepos() ───
DEFAULT_SOURCES = [
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/refs/heads/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/miladtahanian/V2RayCFGDumper/refs/heads/main/sub.txt",
    "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt",
    "https://raw.githubusercontent.com/coldwater-10/V2ray-Config-Lite/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/all_sub.txt",
    "https://raw.githubusercontent.com/M-Mashreghi/Free-V2ray-Collector/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/sub.txt",
    "https://raw.githubusercontent.com/longlon/v2ray-config/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
    "https://raw.githubusercontent.com/freefq/free/master/v2",
    "https://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt",
    "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
    "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge_base64.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/super-sub.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/Sub1.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/Sub2.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/Sub3.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/Sub4.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/Sub5.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/Sub6.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/Sub7.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/Sub8.txt",
    "https://raw.githubusercontent.com/0xRadikal/Free-v2ray-Configs/main/all/configs.txt",
    "https://raw.githubusercontent.com/0xRadikal/Free-v2ray-Configs/main/top100.txt",
    "https://raw.githubusercontent.com/Alirewa/V2ray-Configs/main/config.txt",
    "https://raw.githubusercontent.com/Alirewa/V2ray-Configs/main/sub1.txt",
    "https://raw.githubusercontent.com/hamedcode/port-based-v2ray-configs/main/sub/top100.txt",
    "https://raw.githubusercontent.com/hamedcode/port-based-v2ray-configs/main/sub/vless.txt",
    "https://raw.githubusercontent.com/hamedcode/port-based-v2ray-configs/main/sub/port_443.txt",
    "https://raw.githubusercontent.com/CNT-Cafe/Public-Configs/main/%E2%9C%A7%20Public%20Configs",
    "https://raw.githubusercontent.com/ZendanTeam/SS44-sub/main/SS44-gold.txt",
    "https://raw.githubusercontent.com/ZendanTeam/SS44-sub/main/SS44.txt",
    "https://raw.githubusercontent.com/miladtahanian/Config-Collector/main/mixed_iran.txt",
    "https://raw.githubusercontent.com/mehrtat/vless-collector/main/sub.txt",
    "https://raw.githubusercontent.com/hamedcode/port-based-v2ray-configs/main/sub/clash.yaml",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub1.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub2.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub3.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub4.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub5.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub6.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub7.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub8.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub9.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub10.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub11.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub12.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub13.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub14.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub15.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub16.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub17.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub18.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub19.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub20.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub21.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub22.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub23.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub24.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub25.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub26.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub27.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub28.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub29.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub30.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub31.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub32.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub33.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub34.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub35.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub36.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub37.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub38.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/subs/sub39.txt",
    "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/reality.txt",
    "https://raw.githubusercontent.com/mahdibland/SSAggregator/master/sub/sub_merge.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless",
    # Anti-censorship / Iran-priority extras
    "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge_base64.txt",
]

# URI schemes we accept as valid proxy configs.
VALID_SCHEMES = (
    "vless://", "vmess://", "trojan://", "ss://", "ssr://",
    "hysteria2://", "hy2://", "hysteria://", "tuic://", "snell://",
    "wireguard://", "wg://", "socks://", "socks5://",
)

# Base64 lines: detect by charset (A-Za-z0-9+/=) and length > 40, no "://".
B64_RE = re.compile(r'^[A-Za-z0-9+/=_-]{40,}$')


def fetch_url(url, timeout, proxy):
    """Fetch URL content, return text or empty string on failure."""
    try:
        req = Request(url, headers={
            "User-Agent": "Mozilla/5.0 (Hunter/2.0; config-downloader)",
            "Accept": "*/*",
        })
        if proxy:
            # urllib doesn't make per-request proxy easy; rely on env vars.
            pass
        with urlopen(req, timeout=timeout) as resp:
            data = resp.read()
            # Try utf-8, fall back to latin-1.
            try:
                return data.decode("utf-8")
            except UnicodeDecodeError:
                return data.decode("latin-1", errors="replace")
    except (URLError, HTTPError, TimeoutError, OSError) as e:
        print(f"  [WARN] {url}: {e}", file=sys.stderr)
        return ""
    except Exception as e:
        print(f"  [WARN] {url}: unexpected {e}", file=sys.stderr)
        return ""


def try_base64_decode(line):
    """If a line looks like base64, decode it and return URIs found."""
    line = line.strip()
    if not line or "://" in line:
        return []
    if not B64_RE.match(line):
        return []
    # Try standard and URL-safe base64, with padding fix.
    for variant in (line, line.replace("-", "+").replace("_", "/")):
        padded = variant + "=" * (-len(variant) % 4)
        try:
            decoded = base64.b64decode(padded).decode("utf-8", errors="replace")
            if "://" in decoded:
                return [l.strip() for l in decoded.splitlines() if "://" in l.strip()]
        except Exception:
            continue
    return []


def extract_configs(text, decode_base64):
    """Extract only supported proxy URIs, even when wrapped in HTML or YAML."""
    configs = set()
    uri_re = re.compile(
        r"(?i)(?:vmess|vless|trojan|ssr|ss|hysteria2|hy2|tuic|wireguard)://[^\s\"'<>]+"
    )
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        for match in uri_re.finditer(line):
            candidate = match.group(0).rstrip(",;)]}\\")
            if candidate:
                configs.add(candidate)
        if decode_base64 and "://" not in line:
            for decoded in try_base64_decode(line):
                for match in uri_re.finditer(decoded):
                    candidate = match.group(0).rstrip(",;)]}\\")
                    if candidate:
                        configs.add(candidate)
    return configs


def main():
    ap = argparse.ArgumentParser(description="Download latest proxy configs for embedding.")
    ap.add_argument("--outfile", required=True, help="Output bundle file (one URI per line).")
    ap.add_argument("--timeout", type=int, default=30, help="Per-request timeout (seconds).")
    ap.add_argument("--workers", type=int, default=16, help="Parallel fetch workers.")
    ap.add_argument("--proxy", default=None, help="HTTP/SOCKS proxy URL (sets env).")
    ap.add_argument("--min-configs", type=int, default=100,
                    help="Minimum configs to consider build successful (else exit 1).")
    ap.add_argument("--sources-file", default=None,
                    help="File with one source URL per line (overrides defaults).")
    ap.add_argument("--no-base64-decode", action="store_true",
                    help="Skip base64 line decoding (faster, may miss some sources).")
    ap.add_argument("--merge-existing", default=None,
                    help="Merge with an existing cache file (e.g. runtime/HUNTER_all_cache.txt).")
    args = ap.parse_args()

    # Load sources.
    if args.sources_file:
        with open(args.sources_file) as f:
            sources = [l.strip() for l in f if l.strip() and not l.startswith("#")]
    else:
        sources = list(DEFAULT_SOURCES)

    # Deduplicate sources.
    seen = set()
    sources = [s for s in sources if not (s in seen or seen.add(s))]
    print(f"[download] {len(sources)} sources, {args.workers} workers, timeout={args.timeout}s")

    # Set proxy env if provided.
    if args.proxy:
        os.environ["HTTP_PROXY"] = args.proxy
        os.environ["HTTPS_PROXY"] = args.proxy

    # Fetch all sources in parallel.
    all_configs = set()
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        future_to_url = {
            pool.submit(fetch_url, url, args.timeout, args.proxy): url
            for url in sources
        }
        for future in concurrent.futures.as_completed(future_to_url):
            url = future_to_url[future]
            try:
                text = future.result()
            except Exception as e:
                print(f"  [WARN] {url}: {e}", file=sys.stderr)
                text = ""
            if text:
                found = extract_configs(text, decode_base64=not args.no_base64_decode)
                if found:
                    all_configs.update(found)
                    print(f"  [OK]   {url}: +{len(found)} configs")
                else:
                    print(f"  [EMPTY] {url}: no valid URIs found")
            else:
                print(f"  [FAIL] {url}: no data")

    # Merge with existing cache if requested.
    if args.merge_existing and os.path.exists(args.merge_existing):
        with open(args.merge_existing) as f:
            for line in f:
                line = line.strip()
                if line and "://" in line:
                    all_configs.add(line)
        print(f"[download] Merged with existing cache: {args.merge_existing}")

    elapsed = time.time() - t0
    print(f"[download] Fetched {len(all_configs)} unique configs in {elapsed:.1f}s")

    if len(all_configs) < args.min_configs:
        print(f"[download] ERROR: only {len(all_configs)} configs (min={args.min_configs})",
              file=sys.stderr)
        return 1

    # Write output.
    out = Path(args.outfile)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w") as f:
        for cfg in sorted(all_configs):
            f.write(cfg + "\n")

    size = out.stat().st_size
    print(f"[download] Wrote {len(all_configs)} configs ({size/1024/1024:.1f} MB) to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
