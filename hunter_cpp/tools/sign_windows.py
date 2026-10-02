#!/usr/bin/env python3
"""
sign_windows.py — Code-sign the Hunter Windows executable + extracted engines.

Code signing is the single most effective mitigation against antivirus false
positives on Windows. An Authenticode-signed binary:
  - Shows the publisher name instead of "Unknown Publisher" in SmartScreen
  - Gets a reputation score that reduces heuristic flagging
  - Is trusted by Windows Defender's cloud-based protection

This script wraps signtool.exe (from the Windows SDK) and signs:
  1. The main hunter.exe
  2. (Optionally) the extracted xray.exe and sing-box.exe in bin/

Usage:
  sign_windows.py --exe <path_to_hunter.exe>
                  --cert <pfx_path> --password <pfx_password>
                  [--signtool <path>] [--ts-url <timestamp_server>]
                  [--sign-engines]  # also sign bin/xray.exe, bin/sing-box.exe

Common timestamp servers:
  - http://timestamp.digicert.com (DigiCert, RFC 3161)
  - http://timestamp.sectigo.com  (Sectigo)
  - http://ts.ssl.com             (SSL.com)

For EV (Extended Validation) code signing certificates, use a USB token or
HSM — the private key cannot be exported to a .pfx file. In that case, use
signtool.exe directly with the /csp and /kc options.

AV Whitelisting Process (beyond signing):
  1. Sign the binary (this script).
  2. Upload to VirusTotal (https://www.virustotal.com) — this distributes
     the file to AV vendors for analysis. False positives often clear within
     24-48 hours after a clean signed binary is submitted.
  3. Submit to Microsoft Security Intelligence for false positive review:
     https://www.microsoft.com/en-us/wdsi/filesubmission
  4. For persistent FP issues, contact individual AV vendors via their
     false-positive submission portals.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def find_signtool(explicit: str = None) -> str:
    if explicit:
        return explicit
    # Search common Windows SDK locations.
    candidates = [
        r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\signtool.exe",
        r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.22000.0\x64\signtool.exe",
        r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\signtool.exe",
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    found = shutil.which("signtool")
    if found:
        return found
    return None


def sign_file(signtool: str, file_path: str, cert: str, password: str,
              ts_url: str, description: str = "Hunter") -> bool:
    """Sign a single file with signtool."""
    cmd = [
        signtool, "sign",
        "/f", cert,
        "/p", password,
        "/tr", ts_url,        # RFC 3161 timestamp
        "/td", "sha256",      # Timestamp digest algorithm
        "/fd", "sha256",      # File digest algorithm
        "/d", description,    # Description shown in file properties
        file_path,
    ]
    print(f"[sign] {' '.join(cmd[:6])}... /d {description} {file_path}")
    try:
        subprocess.check_call(cmd)
        print(f"[sign] OK: {file_path}")
        return True
    except subprocess.CalledProcessError as e:
        print(f"[sign] FAILED: {file_path} (exit code {e.returncode})", file=sys.stderr)
        return False


def verify_signature(signtool: str, file_path: str) -> bool:
    """Verify a file's signature."""
    cmd = [signtool, "verify", "/pa", "/all", file_path]
    try:
        subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(f"[sign] Verified: {file_path}")
        return True
    except subprocess.CalledProcessError:
        print(f"[sign] Verification FAILED: {file_path}", file=sys.stderr)
        return False


def main():
    ap = argparse.ArgumentParser(description="Code-sign Hunter Windows executable.")
    ap.add_argument("--exe", required=True, help="Path to hunter.exe")
    ap.add_argument("--cert", required=True, help="Path to .pfx code signing certificate")
    ap.add_argument("--password", required=True, help="PFX password")
    ap.add_argument("--signtool", default=None, help="Path to signtool.exe (auto-detected if omitted)")
    ap.add_argument("--ts-url", default="http://timestamp.digicert.com",
                    help="RFC 3161 timestamp server URL")
    ap.add_argument("--sign-engines", action="store_true",
                    help="Also sign bin/xray.exe and bin/sing-box.exe")
    ap.add_argument("--bin-dir", default=None, help="Directory with engine binaries (for --sign-engines)")
    ap.add_argument("--description", default="Hunter — Anti-Censorship Proxy Tool",
                    help="Description embedded in the signature")
    ap.add_argument("--verify", action="store_true", help="Verify signatures after signing")
    args = ap.parse_args()

    signtool = find_signtool(args.signtool)
    if not signtool:
        print("[sign] ERROR: signtool.exe not found. Install Windows SDK or pass --signtool.",
              file=sys.stderr)
        return 2

    exe_path = Path(args.exe)
    if not exe_path.exists():
        print(f"[sign] ERROR: {exe_path} not found", file=sys.stderr)
        return 1

    all_ok = True

    # Sign main executable.
    if not sign_file(signtool, str(exe_path), args.cert, args.password,
                     args.ts_url, args.description):
        all_ok = False

    # Optionally sign engine binaries.
    if args.sign_engines:
        bin_dir = Path(args.bin_dir) if args.bin_dir else exe_path.parent
        for engine in ["xray.exe", "sing-box.exe"]:
            ep = bin_dir / engine
            if ep.exists():
                desc = f"Xray-core" if engine == "xray.exe" else "sing-box"
                if not sign_file(signtool, str(ep), args.cert, args.password,
                                 args.ts_url, desc):
                    all_ok = False
            else:
                print(f"[sign] SKIP: {ep} not found")

    # Verify.
    if args.verify and all_ok:
        print("\n[sign] Verifying signatures...")
        verify_signature(signtool, str(exe_path))
        if args.sign_engines:
            bin_dir = Path(args.bin_dir) if args.bin_dir else exe_path.parent
            for engine in ["xray.exe", "sing-box.exe"]:
                ep = bin_dir / engine
                if ep.exists():
                    verify_signature(signtool, str(ep))

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
