#!/usr/bin/env python3
"""
Build core engines from upstream source so packaging does not depend on prebuilt binaries.

Targets:
- https://github.com/v2ray/v2ray-core
- https://github.com/sagernet/sing-box
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
THIRD_PARTY = REPO_ROOT / "third_party" / "engines"
BIN_DIR = REPO_ROOT / "bin"

V2RAY_REPO = "https://github.com/v2ray/v2ray-core.git"
SINGBOX_REPO = "https://github.com/sagernet/sing-box.git"


def run(cmd, cwd=None):
    print("[bootstrap]", " ".join(cmd))
    subprocess.check_call(cmd, cwd=str(cwd) if cwd else None)


def ensure_repo(path: Path, remote: str, branch: str = "main"):
    if path.exists() and (path / ".git").exists():
        run(["git", "fetch", "--depth", "1", "origin", branch], cwd=path)
        run(["git", "checkout", branch], cwd=path)
        run(["git", "reset", "--hard", f"origin/{branch}"], cwd=path)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "clone", "--depth", "1", "--branch", branch, remote, str(path)])


def build_v2ray():
    repo = THIRD_PARTY / "v2ray-core"
    ensure_repo(repo, V2RAY_REPO, "master")
    env = os.environ.copy()
    env["CGO_ENABLED"] = "0"
    out = BIN_DIR / "v2ray.exe"
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    run(["go", "build", "-trimpath", "-ldflags", "-s -w", "-o", str(out), "./main"], cwd=repo)

    # Compatibility alias for existing code path expecting xray.exe.
    xray_alias = BIN_DIR / "xray.exe"
    shutil.copyfile(out, xray_alias)
    print(f"[bootstrap] built {out} and alias {xray_alias}")


def build_singbox():
    repo = THIRD_PARTY / "sing-box"
    ensure_repo(repo, SINGBOX_REPO, "main")
    env = os.environ.copy()
    env["CGO_ENABLED"] = "0"
    out = BIN_DIR / "sing-box.exe"
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    run(["go", "build", "-trimpath", "-tags", "with_quic,with_grpc,with_utls", "-ldflags", "-s -w",
         "-o", str(out), "./cmd/sing-box"], cwd=repo)
    print(f"[bootstrap] built {out}")


def main():
    if shutil.which("git") is None:
        print("[bootstrap] ERROR: git not found", file=sys.stderr)
        return 2
    if shutil.which("go") is None:
        print("[bootstrap] ERROR: go not found", file=sys.stderr)
        return 2

    build_v2ray()
    build_singbox()
    print("[bootstrap] core engines are ready in bin/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
