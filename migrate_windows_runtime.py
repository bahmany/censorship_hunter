#!/usr/bin/env python3
"""
Migrate valuable runtime assets from Windows installation to Docker production runtime.
Performs deduplication and validation of imported records.

Usage (run on remote server with access to Docker container):
  python3 migrate_windows_runtime.py --windows-dir /tmp/windows_runtime --docker-container hunter-backend-dev
"""

import argparse
import os
import sys
import tempfile
import subprocess
import json
from collections import defaultdict
from datetime import datetime


def parse_config_db_tsv(filepath):
    """Parse HUNTER_config_db.tsv into a dict keyed by URI."""
    records = {}
    total = 0
    invalid = 0
    
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        header = f.readline()
        if not header.startswith('#HUNTER_CONFIG_DB'):
            # Maybe no header, rewind
            f.seek(0)
        
        for line in f:
            total += 1
            line = line.strip()
            if not line:
                continue
            
            parts = line.split('\t')
            if len(parts) < 2:
                invalid += 1
                continue
            
            uri = parts[0].strip()
            if not uri or '://' not in uri:
                invalid += 1
                continue
            
            if uri in records:
                # Keep the one with more tests or more recent
                existing = records[uri]
                # Fields: uri, source, engine, first_seen, last_tested, latency, total_tests, total_passes, last_alive, consecutive_fails, tag, alive
                try:
                    existing_tests = int(parts[6]) if len(parts) > 6 else 0
                    existing_tests_prev = int(existing[6]) if len(existing) > 6 else 0
                    if existing_tests > existing_tests_prev:
                        records[uri] = parts
                except (ValueError, IndexError):
                    pass
            else:
                records[uri] = parts
    
    return records, total, invalid


def parse_simple_config_list(filepath):
    """Parse simple URI list files (gold.txt, silver.txt)."""
    uris = set()
    total = 0
    invalid = 0
    
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            total += 1
            line = line.strip()
            if not line:
                continue
            if '://' not in line:
                invalid += 1
                continue
            uris.add(line)
    
    return uris, total, invalid


def merge_config_dbs(windows_records, docker_records):
    """Merge two config DB dicts, keeping best record per URI."""
    merged = dict(docker_records)  # Start with Docker
    new_count = 0
    updated_count = 0
    
    for uri, parts in windows_records.items():
        if uri not in merged:
            merged[uri] = parts
            new_count += 1
        else:
            # Compare and keep better record
            existing = merged[uri]
            try:
                w_tests = int(parts[6]) if len(parts) > 6 else 0
                d_tests = int(existing[6]) if len(existing) > 6 else 0
                if w_tests > d_tests:
                    merged[uri] = parts
                    updated_count += 1
            except (ValueError, IndexError):
                pass
    
    return merged, new_count, updated_count


def write_config_db_tsv(filepath, records):
    """Write merged config DB TSV."""
    with open(filepath, 'w', encoding='utf-8') as f:
        f.write('#HUNTER_CONFIG_DB_V2\n')
        for uri in sorted(records.keys()):
            parts = records[uri]
            f.write('\t'.join(parts) + '\n')


def merge_simple_list(filepath, existing_uris, new_uris):
    """Merge simple URI list files."""
    merged = existing_uris | new_uris
    with open(filepath, 'w', encoding='utf-8') as f:
        for uri in sorted(merged):
            f.write(uri + '\n')
    return len(merged), len(new_uris - existing_uris)


def merge_json_cache(windows_path, docker_path):
    """Merge balancer cache JSON files (they contain config lists with latencies)."""
    windows_configs = {}
    docker_configs = {}
    
    try:
        with open(windows_path, 'r') as f:
            data = json.load(f)
            if isinstance(data, list):
                for item in data:
                    if isinstance(item, list) and len(item) >= 2:
                        windows_configs[item[0]] = item[1]
            elif isinstance(data, dict):
                for k, v in data.items():
                    windows_configs[k] = v
    except (json.JSONDecodeError, FileNotFoundError):
        pass
    
    try:
        with open(docker_path, 'r') as f:
            data = json.load(f)
            if isinstance(data, list):
                for item in data:
                    if isinstance(item, list) and len(item) >= 2:
                        docker_configs[item[0]] = item[1]
            elif isinstance(data, dict):
                for k, v in data.items():
                    docker_configs[k] = v
    except (json.JSONDecodeError, FileNotFoundError):
        pass
    
    merged = dict(docker_configs)
    new = 0
    for uri, latency in windows_configs.items():
        if uri not in merged:
            merged[uri] = latency
            new += 1
    
    # Write merged as list of [uri, latency] pairs sorted by latency
    with open(docker_path, 'w', encoding='utf-8') as f:
        items = sorted(merged.items(), key=lambda x: x[1] if isinstance(x[1], (int, float)) else 9999)
        json.dump([[uri, lat] for uri, lat in items], f, indent=2)
    
    return len(merged), new


def run_migration(windows_dir, docker_container):
    """Main migration function."""
    print("=" * 60)
    print("Hunter Runtime Migration: Windows -> Docker Production")
    print("=" * 60)
    print(f"Windows source: {windows_dir}")
    print(f"Docker container: {docker_container}")
    print()
    
    report = {
        "timestamp": datetime.now().isoformat(),
        "windows_dir": windows_dir,
        "docker_container": docker_container,
        "files": {}
    }
    
    # Step 1: Copy files from Windows dir to Docker container
    print("[1/5] Copying runtime files to Docker container...")
    
    files_to_migrate = [
        ("HUNTER_config_db.tsv", "config_db"),
        ("HUNTER_gold.txt", "gold"),
        ("HUNTER_silver.txt", "silver"),
        ("HUNTER_balancer_cache.json", "balancer_cache"),
        ("HUNTER_gemini_balancer_cache.json", "gemini_balancer_cache"),
        ("HUNTER_all_cache.txt", "all_cache"),
        ("HUNTER_github_configs_cache.txt", "github_cache"),
        ("seed_configs.txt", "seed_configs"),
        ("sources_manager.tsv", "sources_manager"),
        ("source_history.tsv", "source_history"),
    ]
    
    tmpdir = tempfile.mkdtemp(prefix="hunter_migration_")
    
    for filename, label in files_to_migrate:
        src = os.path.join(windows_dir, filename)
        if not os.path.exists(src):
            print(f"  SKIP {filename} (not found)")
            continue
        
        # Copy to temp dir
        dst = os.path.join(tmpdir, filename)
        import shutil
        shutil.copy2(src, dst)
        
        # Copy to Docker container
        docker_path = f"/app/hunter_cpp/runtime/{filename}"
        result = subprocess.run(
            ["docker", "cp", dst, f"{docker_container}:{docker_path}"],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            print(f"  FAIL {filename}: {result.stderr.strip()}")
        else:
            size = os.path.getsize(src)
            print(f"  OK   {filename} ({size:,} bytes)")
    
    print()
    
    # Step 2: Merge config databases
    print("[2/5] Merging config databases...")
    
    windows_db_path = os.path.join(tmpdir, "HUNTER_config_db.tsv")
    if os.path.exists(windows_db_path):
        # Get Docker's current DB
        docker_db_path = os.path.join(tmpdir, "docker_config_db.tsv")
        result = subprocess.run(
            ["docker", "cp", f"{docker_container}:/app/hunter_cpp/runtime/HUNTER_config_db.tsv", docker_db_path],
            capture_output=True, text=True
        )
        
        if result.returncode == 0 and os.path.exists(docker_db_path):
            windows_records, w_total, w_invalid = parse_config_db_tsv(windows_db_path)
            docker_records, d_total, d_invalid = parse_config_db_tsv(docker_db_path)
            
            merged, new_count, updated_count = merge_config_dbs(windows_records, docker_records)
            
            merged_path = os.path.join(tmpdir, "merged_config_db.tsv")
            write_config_db_tsv(merged_path, merged)
            
            # Copy merged DB back to Docker
            subprocess.run(
                ["docker", "cp", merged_path, f"{docker_container}:/app/hunter_cpp/runtime/HUNTER_config_db.tsv"],
                capture_output=True, text=True
            )
            
            stats = {
                "windows_total": w_total,
                "windows_valid": len(windows_records),
                "windows_invalid": w_invalid,
                "docker_total": d_total,
                "docker_valid": len(docker_records),
                "docker_invalid": d_invalid,
                "merged_total": len(merged),
                "new_from_windows": new_count,
                "updated_records": updated_count,
                "duplicates_removed": w_total - len(windows_records) - w_invalid,
            }
            report["files"]["config_db"] = stats
            print(f"  Windows: {w_total} total, {len(windows_records)} valid, {w_invalid} invalid")
            print(f"  Docker:  {d_total} total, {len(docker_records)} valid, {d_invalid} invalid")
            print(f"  Merged:  {len(merged)} unique records ({new_count} new, {updated_count} updated)")
            print(f"  Duplicates removed: {stats['duplicates_removed']}")
        else:
            print("  Could not read Docker DB, using Windows DB directly")
    else:
        print("  No Windows config DB found")
    
    print()
    
    # Step 3: Merge gold/silver files
    print("[3/5] Merging gold and silver config lists...")
    
    for filename, label in [("HUNTER_gold.txt", "gold"), ("HUNTER_silver.txt", "silver")]:
        win_path = os.path.join(tmpdir, filename)
        if not os.path.exists(win_path):
            print(f"  SKIP {filename} (not found)")
            continue
        
        docker_path = os.path.join(tmpdir, f"docker_{filename}")
        result = subprocess.run(
            ["docker", "cp", f"{docker_container}:/app/hunter_cpp/runtime/{filename}", docker_path],
            capture_output=True, text=True
        )
        
        existing = set()
        if result.returncode == 0 and os.path.exists(docker_path):
            existing, _, _ = parse_simple_config_list(docker_path)
        
        new_uris, n_total, n_invalid = parse_simple_config_list(win_path)
        merged_count, added = merge_simple_list(win_path, existing, new_uris)
        
        # Copy merged back to Docker
        subprocess.run(
            ["docker", "cp", win_path, f"{docker_container}:/app/hunter_cpp/runtime/{filename}"],
            capture_output=True, text=True
        )
        
        stats = {
            "windows_total": n_total,
            "windows_valid": len(new_uris),
            "windows_invalid": n_invalid,
            "docker_existing": len(existing),
            "merged_total": merged_count,
            "new_added": added,
        }
        report["files"][label] = stats
        print(f"  {filename}: {n_total} Windows -> {merged_count} merged ({added} new)")
    
    print()
    
    # Step 4: Merge balancer caches
    print("[4/5] Merging balancer cache files...")
    
    for filename, label in [("HUNTER_balancer_cache.json", "balancer_cache"),
                            ("HUNTER_gemini_balancer_cache.json", "gemini_balancer_cache")]:
        win_path = os.path.join(tmpdir, filename)
        if not os.path.exists(win_path):
            print(f"  SKIP {filename} (not found)")
            continue
        
        docker_path = os.path.join(tmpdir, f"docker_{filename}")
        result = subprocess.run(
            ["docker", "cp", f"{docker_container}:/app/hunter_cpp/runtime/{filename}", docker_path],
            capture_output=True, text=True
        )
        
        if result.returncode != 0 or not os.path.exists(docker_path):
            # Just use Windows version
            subprocess.run(
                ["docker", "cp", win_path, f"{docker_container}:/app/hunter_cpp/runtime/{filename}"],
                capture_output=True, text=True
            )
            print(f"  {filename}: Using Windows version directly")
        else:
            merged_total, new = merge_json_cache(win_path, docker_path)
            subprocess.run(
                ["docker", "cp", docker_path, f"{docker_container}:/app/hunter_cpp/runtime/{filename}"],
                capture_output=True, text=True
            )
            report["files"][label] = {"merged_total": merged_total, "new_from_windows": new}
            print(f"  {filename}: {merged_total} merged ({new} new from Windows)")
    
    print()
    
    # Step 5: Summary
    print("[5/5] Migration Summary")
    print("=" * 60)
    
    total_records = 0
    for label, stats in report["files"].items():
        if "merged_total" in stats:
            total_records = max(total_records, stats["merged_total"])
    
    print(f"  Config DB: {report['files'].get('config_db', {}).get('merged_total', 'N/A')} unique records")
    print(f"  Gold:      {report['files'].get('gold', {}).get('merged_total', 'N/A')} configs")
    print(f"  Silver:    {report['files'].get('silver', {}).get('merged_total', 'N/A')} configs")
    print(f"  Balancer:  {report['files'].get('balancer_cache', {}).get('merged_total', 'N/A')} cached configs")
    
    # Save report
    report_path = os.path.join(tmpdir, "migration_report.json")
    with open(report_path, 'w') as f:
        json.dump(report, f, indent=2)
    
    # Copy report to Docker
    subprocess.run(
        ["docker", "cp", report_path, f"{docker_container}:/app/hunter_cpp/runtime/migration_report.json"],
        capture_output=True, text=True
    )
    
    print(f"\n  Report saved to Docker: /app/hunter_cpp/runtime/migration_report.json")
    print(f"  Temp dir: {tmpdir}")
    print("=" * 60)
    
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Migrate Windows runtime to Docker production")
    parser.add_argument("--windows-dir", required=True, help="Directory containing Windows runtime files")
    parser.add_argument("--docker-container", default="hunter-backend-dev", help="Docker container name")
    
    args = parser.parse_args()
    run_migration(args.windows_dir, args.docker_container)
