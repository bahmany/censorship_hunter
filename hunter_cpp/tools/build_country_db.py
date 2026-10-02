#!/usr/bin/env python3
"""
build_country_db.py — Download DB-IP Lite Country CSV and build HCGEO1 binary database.

Converts DB-IP Country Lite CSV into the custom HCGEO1 binary format:
- Merges adjacent contiguous same-country address ranges
- Country dictionary with index 0 = Unknown ("ZZ")
- Header: magic "HCGEO1", version, endian, IPv4/IPv6 counts, source date, SHA-256 of payload
- IPv4 records: start (u32 LE), end (u32 LE), country (u16 LE) = 10 bytes
- IPv6 records: start (16B big-endian), end (16B big-endian), country (u16 LE) = 34 bytes
- Zstandard compression (-19)
- Optional single-file embedding generation (.h, .S, .json)

License notice:
  IP Geolocation by DB-IP (https://db-ip.com)
  Licensed under Creative Commons Attribution 4.0 International (CC BY 4.0).
"""

import argparse
import datetime
import gzip
import hashlib
import ipaddress
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path


def get_current_and_prev_ym():
    now = datetime.datetime.now(datetime.timezone.utc)
    cur = now.strftime("%Y-%m")
    if now.month == 1:
        prev = f"{now.year - 1:04d}-12"
    else:
        prev = f"{now.year:04d}-{now.month - 1:02d}"
    return cur, prev


def download_csv(dest_path: Path, requested_ym: str = None) -> str:
    """Download DB-IP Lite Country CSV, returning the source YYYY-MM."""
    if requested_ym:
        candidates = [requested_ym]
    else:
        cur_ym, prev_ym = get_current_and_prev_ym()
        candidates = [cur_ym, prev_ym]

    headers = {
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) CensorshipHunter/2.0"
    }

    for ym in candidates:
        url = f"https://download.db-ip.com/free/dbip-country-lite-{ym}.csv.gz"
        print(f"[geo-build] Attempting download: {url}")
        req = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=30) as resp, open(dest_path, "wb") as out_f:
                shutil.copyfileobj(resp, out_f)
            file_size = dest_path.stat().st_size
            print(f"[geo-build] Successfully downloaded {ym} ({file_size} bytes)")
            return ym
        except urllib.error.HTTPError as e:
            print(f"[geo-build] HTTP error {e.code} for {ym}: {e.reason}")
        except Exception as e:
            print(f"[geo-build] Download error for {ym}: {e}")

    raise RuntimeError(f"Failed to download DB-IP Lite Country CSV for candidates: {candidates}")


def merge_ranges(records):
    """Merge contiguous adjacent ranges with the same country code."""
    if not records:
        return []
    records.sort(key=lambda r: r[0])
    merged = [records[0]]
    for start, end, country in records[1:]:
        p_start, p_end, p_country = merged[-1]
        if country == p_country and start == p_end + 1:
            merged[-1] = (p_start, end, p_country)
        else:
            merged.append((start, end, country))
    return merged


def parse_csv_data(csv_path: Path):
    """Parse CSV and extract IPv4, IPv6 records and country dictionary."""
    is_gz = str(csv_path).endswith(".gz")
    open_fn = gzip.open if is_gz else open

    v4_records = []
    v6_records = []
    countries = set()

    line_count = 0
    with open_fn(csv_path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            line_count += 1
            line = line.strip()
            if not line:
                continue
            parts = line.split(",")
            if len(parts) < 3:
                continue

            start_str, end_str, country = parts[0].strip(), parts[1].strip(), parts[2].strip()
            if not country or len(country) != 2 or not country.isalpha():
                country = "ZZ"
            else:
                country = country.upper()

            countries.add(country)

            if ":" in start_str:
                start_int = int(ipaddress.IPv6Address(start_str))
                end_int = int(ipaddress.IPv6Address(end_str))
                v6_records.append((start_int, end_int, country))
            else:
                start_int = int(ipaddress.IPv4Address(start_str))
                end_int = int(ipaddress.IPv4Address(end_str))
                v4_records.append((start_int, end_int, country))

    print(f"[geo-build] Read {line_count} CSV lines.")
    print(f"[geo-build] Raw records: IPv4={len(v4_records)}, IPv6={len(v6_records)}, Countries={len(countries)}")

    v4_merged = merge_ranges(v4_records)
    v6_merged = merge_ranges(v6_records)

    merged_count = (len(v4_records) - len(v4_merged)) + (len(v6_records) - len(v6_merged))
    print(f"[geo-build] Merged adjacent same-country ranges: reduced by {merged_count} records.")
    print(f"[geo-build] Post-merge records: IPv4={len(v4_merged)}, IPv6={len(v6_merged)}")

    # Index 0 is reserved for Unknown ("ZZ")
    sorted_countries = sorted([c for c in countries if c != "ZZ"])
    country_list = ["ZZ"] + sorted_countries
    c2idx = {c: i for i, c in enumerate(country_list)}

    return v4_merged, v6_merged, country_list, c2idx


def build_hcgeo1_binary(v4_records, v6_records, country_list, c2idx, source_ym: str) -> bytes:
    """Build uncompressed HCGEO1 binary payload and header."""
    # 1. Country dictionary: country_count * 2 bytes
    dict_bytes = bytearray()
    for c in country_list:
        dict_bytes.extend(c.encode("ascii")[:2])

    # 2. IPv4 records: start (u32 LE), end (u32 LE), country (u16 LE) = 10 bytes each
    v4_bytes = bytearray()
    for start, end, country in v4_records:
        v4_bytes.extend(struct.pack("<IIH", start, end, c2idx[country]))

    # 3. IPv6 records: start (16B big-endian), end (16B big-endian), country (u16 LE) = 34 bytes each
    v6_bytes = bytearray()
    for start, end, country in v6_records:
        v6_bytes.extend(start.to_bytes(16, "big"))
        v6_bytes.extend(end.to_bytes(16, "big"))
        v6_bytes.extend(struct.pack("<H", c2idx[country]))

    payload = bytes(dict_bytes) + bytes(v4_bytes) + bytes(v6_bytes)
    payload_sha256 = hashlib.sha256(payload).digest()

    # Header layout (70 bytes):
    # magic: 6B ('HCGEO1')
    # version: u16 (1)
    # endian: u8 (1 = Little-endian)
    # reserved: u8 (0)
    # ipv4_count: u32
    # ipv6_count: u32
    # country_count: u16
    # reserved2: u16 (0)
    # source_date: 16B (null-padded ASCII)
    # sha256: 32B
    source_date_bytes = source_ym.encode("ascii")[:15].ljust(16, b"\x00")
    header = struct.pack(
        "<6sHBBIIHH16s32s",
        b"HCGEO1",
        1,
        1,
        0,
        len(v4_records),
        len(v6_records),
        len(country_list),
        0,
        source_date_bytes,
        payload_sha256,
    )
    return header + payload


def compress_zstd(raw_bytes: bytes, zstd_bin: str = "zstd") -> bytes:
    """Compress data using zstd -19."""
    if shutil.which(zstd_bin):
        p = subprocess.Popen(
            [zstd_bin, "-19", "-f", "-q", "-c"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        compressed, stderr = p.communicate(raw_bytes)
        if p.returncode == 0 and compressed:
            return compressed

    # Fallback to python-zstandard if installed
    try:
        import zstandard as zstd
        cctx = zstd.ZstdCompressor(level=19)
        return cctx.compress(raw_bytes)
    except ImportError:
        pass

    raise RuntimeError("Neither 'zstd' executable nor 'zstandard' Python package available for compression.")


def decompress_zstd(compressed_bytes: bytes, zstd_bin: str = "zstd") -> bytes:
    """Decompress data using zstd CLI or python library."""
    if shutil.which(zstd_bin):
        p = subprocess.Popen(
            [zstd_bin, "-d", "-c", "-q"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        decomp, stderr = p.communicate(compressed_bytes)
        if p.returncode == 0 and decomp:
            return decomp

    try:
        import zstandard as zstd
        dctx = zstd.ZstdDecompressor()
        return dctx.decompress(compressed_bytes)
    except ImportError:
        pass

    raise RuntimeError("Neither 'zstd' executable nor 'zstandard' Python package available for decompression.")


def handle_embed_existing_db(db_path: Path, embed_dir: Path, platform: str, zstd_bin: str) -> int:
    """Embed an existing HCGEO1 database file into C++ header/assembly."""
    data = db_path.read_bytes()
    if len(data) >= 4 and data[:4] == b"\x28\xb5\x2f\xfd":
        compressed_data = data
        comp_size = len(compressed_data)
        raw_data = decompress_zstd(compressed_data, zstd_bin)
        raw_size = len(raw_data)
    else:
        raw_data = data
        raw_size = len(raw_data)
        compressed_data = compress_zstd(raw_data, zstd_bin)
        comp_size = len(compressed_data)

    if len(raw_data) < 70 or raw_data[:6] != b"HCGEO1":
        print(f"[geo-build] Error: {db_path} is not a valid HCGEO1 database", file=sys.stderr)
        return 1

    magic, version, endian, res, v4_count, v6_count, country_count, res2, src_date, sha256_bytes = struct.unpack(
        "<6sHBBIIHH16s32s", raw_data[:70]
    )
    src_date_str = src_date.decode("ascii", errors="replace").rstrip("\x00 ")
    raw_digest = hashlib.sha256(raw_data).hexdigest()

    embed_dir.mkdir(parents=True, exist_ok=True)
    embed_blob_name = "country.zst"
    (embed_dir / embed_blob_name).write_bytes(compressed_data)

    generate_embed_files(
        embed_dir,
        embed_blob_name,
        comp_size,
        raw_size,
        raw_digest,
        src_date_str,
        v4_count,
        v6_count,
        country_count,
        platform,
    )
    return 0


def generate_embed_files(out_dir: Path, zst_filename: str, comp_size: int, raw_size: int,
                         digest: str, source_ym: str, v4_count: int, v6_count: int,
                         country_count: int, platform: str):
    """Generate C++ embed header, assembly .S file, and metadata JSON."""
    out_dir.mkdir(parents=True, exist_ok=True)
    target_win = (platform or sys.platform).lower() in ("windows", "win", "win32", "mingw")

    # Header
    header_lines = [
        "// AUTO-GENERATED by tools/build_country_db.py — DO NOT EDIT.",
        "// Embedded offline country database for Censorship Hunter.",
        "// Data source: IP Geolocation by DB-IP (https://db-ip.com), CC BY 4.0.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "#define HUNTER_EMBED_HAS_GEO 1",
        "#define HUNTER_EMBED_GEO_USE_ZSTD 1",
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        "extern const unsigned char kGeoZst[];",
        "extern const uint64_t      kGeoZstLen;",
        f"static const uint64_t      kGeoOrigLen = {raw_size}ULL;",
        f'static const char          kGeoSha256[] = "{digest}";',
        f'static const char          kGeoSourceDate[] = "{source_ym}";',
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
    ]
    (out_dir / "geo_embedded.h").write_text("\n".join(header_lines))

    # Assembler file .S
    asm_lines = [
        "// AUTO-GENERATED by tools/build_country_db.py — DO NOT EDIT.",
        "// Embedded country database via .incbin.",
        "",
    ]
    if target_win:
        asm_lines.append('    .section .hunterdat,"dr"')
    else:
        asm_lines.append("    .section .rodata")
    asm_lines.append("")
    asm_lines.append(f"// ─── Country Database ({comp_size} bytes compressed, {raw_size} bytes raw) ───")
    asm_lines.append("    .global kGeoZst")
    asm_lines.append("kGeoZst:")
    asm_lines.append(f'    .incbin "{zst_filename}"')
    asm_lines.append("    .balign 8")
    asm_lines.append("    .global kGeoZstLen")
    asm_lines.append("kGeoZstLen:")
    asm_lines.append(f"    .quad {comp_size}")
    asm_lines.append("")
    if not target_win:
        asm_lines.append('    .section .note.GNU-stack,"",@progbits')
        asm_lines.append("")
    (out_dir / "geo_embedded.S").write_text("\n".join(asm_lines))

    # Metadata JSON
    meta = {
        "app": "Hunter Censorship Hunter",
        "kind": "offline_geo_db",
        "format": "HCGEO1",
        "source_date": source_ym,
        "ipv4_count": v4_count,
        "ipv6_count": v6_count,
        "country_count": country_count,
        "original_size": raw_size,
        "compressed_size": comp_size,
        "sha256": digest,
        "license": "Creative Commons Attribution 4.0 International (CC BY 4.0)",
        "attribution": "IP Geolocation by DB-IP (https://db-ip.com)",
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }
    (out_dir / "geo_meta.json").write_text(json.dumps(meta, indent=2))
    print(f"[geo-build] Generated embed files in {out_dir}")


def main():
    parser = argparse.ArgumentParser(description="Download DB-IP Lite Country CSV and build HCGEO1 binary database.")
    parser.add_argument("--out", default="country.bin.zst", help="Output path for compressed database (default: country.bin.zst)")
    parser.add_argument("--raw-out", default=None, help="Optional output path for uncompressed raw HCGEO1 database")
    parser.add_argument("--csv", default=None, help="Path to existing CSV or CSV.gz file (skips download)")
    parser.add_argument("--year-month", default=None, help="Specific DB-IP release YYYY-MM to fetch (e.g. 2026-10)")
    parser.add_argument("--embed-outdir", default=None, help="Output directory to generate C++ embed files (.h, .S, .json)")
    parser.add_argument("--embed-db", default=None, help="Path to existing HCGEO1 database (.bin or .bin.zst) to embed")
    parser.add_argument("--platform", default=None, help="Target platform ('linux' or 'windows')")
    parser.add_argument("--zstd", default=shutil.which("zstd") or "zstd", help="zstd CLI path")
    parser.add_argument("--no-zstd", action="store_true", help="Do not compress with zstd")
    args = parser.parse_args()

    if args.embed_db:
        embed_db_path = Path(args.embed_db)
        if not embed_db_path.exists():
            print(f"[geo-build] Error: --embed-db file not found: {embed_db_path}", file=sys.stderr)
            return 1
        if not args.embed_outdir:
            print("[geo-build] Error: --embed-db requires --embed-outdir", file=sys.stderr)
            return 1
        return handle_embed_existing_db(embed_db_path, Path(args.embed_outdir), args.platform, args.zstd)

    source_ym = args.year_month
    temp_dir = None
    try:
        if args.csv:
            csv_path = Path(args.csv)
            if not csv_path.exists():
                print(f"[geo-build] Error: CSV file not found: {csv_path}", file=sys.stderr)
                return 1
            if not source_ym:
                source_ym = "unknown"
        else:
            temp_dir = tempfile.TemporaryDirectory()
            dl_path = Path(temp_dir.name) / "dbip-country-lite.csv.gz"
            source_ym = download_csv(dl_path, args.year_month)
            csv_path = dl_path

        v4_records, v6_records, country_list, c2idx = parse_csv_data(csv_path)

        raw_data = build_hcgeo1_binary(v4_records, v6_records, country_list, c2idx, source_ym)
        raw_size = len(raw_data)
        raw_digest = hashlib.sha256(raw_data).hexdigest()

        if args.raw_out:
            raw_out_path = Path(args.raw_out)
            raw_out_path.parent.mkdir(parents=True, exist_ok=True)
            raw_out_path.write_bytes(raw_data)
            print(f"[geo-build] Wrote uncompressed database to {raw_out_path}")

        out_path = Path(args.out)
        out_path.parent.mkdir(parents=True, exist_ok=True)

        if not args.no_zstd:
            compressed_data = compress_zstd(raw_data, args.zstd)
            comp_size = len(compressed_data)
            out_path.write_bytes(compressed_data)
            comp_digest = hashlib.sha256(compressed_data).hexdigest()
        else:
            compressed_data = None
            comp_size = raw_size
            comp_digest = raw_digest
            out_path.write_bytes(raw_data)

        ratio = (comp_size / raw_size * 100.0) if raw_size else 0.0

        # Print measured sizes
        print("=" * 60)
        print("HCGEO1 Database Build Summary:")
        print(f"  Source Date:            {source_ym}")
        print(f"  Unique Countries:       {len(country_list)}")
        print(f"  IPv4 Records:           {len(v4_records):,}")
        print(f"  IPv6 Records:           {len(v6_records):,}")
        print(f"  Raw Binary Size:        {raw_size:,} bytes ({raw_size / (1024 * 1024):.2f} MB)")
        print(f"  Compressed Size (zstd): {comp_size:,} bytes ({comp_size / (1024 * 1024):.2f} MB)")
        print(f"  Compression Ratio:      {ratio:.1f}%")
        print(f"  Raw SHA-256:            {raw_digest}")
        print(f"  Output File:            {out_path}")
        print("=" * 60)

        if args.embed_outdir:
            embed_dir = Path(args.embed_outdir)
            embed_dir.mkdir(parents=True, exist_ok=True)
            # Copy or write compressed blob into embed dir
            ext = ".zst" if not args.no_zstd else ".bin"
            embed_blob_name = f"country{ext}"
            embed_blob_path = embed_dir / embed_blob_name
            if not args.no_zstd:
                embed_blob_path.write_bytes(compressed_data)
            else:
                embed_blob_path.write_bytes(raw_data)

            generate_embed_files(
                embed_dir,
                embed_blob_name,
                comp_size,
                raw_size,
                raw_digest,
                source_ym,
                len(v4_records),
                len(v6_records),
                len(country_list),
                args.platform,
            )

    finally:
        if temp_dir is not None:
            temp_dir.cleanup()

    return 0


if __name__ == "__main__":
    sys.exit(main())
