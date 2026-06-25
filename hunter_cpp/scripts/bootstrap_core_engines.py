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


MIHOMO_REPO = "https://github.com/MetaCubeX/mihomo.git"

TOR_URL = "https://dist.torproject.org/tor-0.4.8.12.tar.gz"


def run(cmd, cwd=None, env=None):
    print("[bootstrap]", " ".join(cmd))
    subprocess.check_call(cmd, cwd=str(cwd) if cwd else None, env=env)


def ensure_repo(path: Path, remote: str, branch: str = "main"):
    if path.exists() and (path / ".git").exists():
        try:
            run(["git", "fetch", "--depth", "1", "origin", branch], cwd=path)
            run(["git", "checkout", branch], cwd=path)
            run(["git", "reset", "--hard", f"origin/{branch}"], cwd=path)
            return
        except subprocess.CalledProcessError:
            shutil.rmtree(path, ignore_errors=True)
    path.parent.mkdir(parents=True, exist_ok=True)
    mirrors = [remote]
    if "github.com" in remote:
        mirrors.append(remote.replace("github.com", "hub.fastgit.xyz"))
        mirrors.append(remote.replace("github.com", "hub.0z.gs"))
    for mirror in mirrors:
        try:
            run(["git", "clone", "--depth", "1", "--branch", branch, mirror, str(path)])
            return
        except subprocess.CalledProcessError:
            print(f"[bootstrap] clone failed from {mirror}, trying next...")
            shutil.rmtree(path, ignore_errors=True)
    raise RuntimeError(f"Failed to clone {remote} from all mirrors")


def build_v2ray():
    repo = THIRD_PARTY / "v2ray-core"
    ensure_repo(repo, V2RAY_REPO, "master")
    env = os.environ.copy()
    env["CGO_ENABLED"] = "0"
    env["GOPROXY"] = "https://goproxy.cn,direct"
    is_windows = sys.platform == "win32"
    ext = ".exe" if is_windows else ""
    out = BIN_DIR / f"v2ray{ext}"
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    run(["go", "build", "-trimpath", "-ldflags", "-s -w", "-o", str(out), "./main"], cwd=repo, env=env)

    # Compatibility alias for existing code path expecting xray.
    xray_alias = BIN_DIR / f"xray{ext}"
    shutil.copyfile(out, xray_alias)
    print(f"[bootstrap] built {out} and alias {xray_alias}")


def build_singbox():
    repo = THIRD_PARTY / "sing-box"
    try:
        ensure_repo(repo, SINGBOX_REPO, "main")
    except RuntimeError:
        print("[bootstrap] sing-box clone failed, trying pre-built binary download...")
        is_windows = sys.platform == "win32"
        ext = ".exe" if is_windows else ""
        out = BIN_DIR / f"sing-box{ext}"
        BIN_DIR.mkdir(parents=True, exist_ok=True)
        import platform as _pf
        arch = "amd64" if _pf.machine() in ("x86_64", "amd64") else "arm64"
        urls = [
            f"https://github.com/SagerNet/sing-box/releases/download/v1.8.10/sing-box-1.8.10-linux-{arch}.tar.gz",
            f"https://hub.0z.gs/SagerNet/sing-box/releases/download/v1.8.10/sing-box-1.8.10-linux-{arch}.tar.gz",
        ]
        import tempfile, tarfile
        for url in urls:
            try:
                tmpdir = Path(tempfile.mkdtemp())
                archive = tmpdir / "singbox.tar.gz"
                run(["wget", "-q", "-O", str(archive), url])
                with tarfile.open(archive, "r:gz") as tar:
                    tar.extractall(tmpdir)
                built = next(tmpdir.glob("sing-box-*")) / "sing-box"
                shutil.copyfile(str(built), str(out))
                os.chmod(str(out), 0o755)
                print(f"[bootstrap] downloaded sing-box binary to {out}")
                return
            except Exception as e:
                print(f"[bootstrap] download from {url} failed: {e}")
        raise RuntimeError("Failed to get sing-box from all sources")
    env = os.environ.copy()
    env["CGO_ENABLED"] = "0"
    env["GOPROXY"] = "https://goproxy.cn,direct"
    is_windows = sys.platform == "win32"
    ext = ".exe" if is_windows else ""
    out = BIN_DIR / f"sing-box{ext}"
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    run(["go", "build", "-trimpath", "-tags", "with_quic,with_grpc,with_utls", "-ldflags", "-s -w",
         "-o", str(out), "./cmd/sing-box"], cwd=repo, env=env)
    print(f"[bootstrap] built {out}")


def build_mihomo():
    repo = THIRD_PARTY / "mihomo"
    try:
        ensure_repo(repo, MIHOMO_REPO, "Meta")
    except RuntimeError:
        print("[bootstrap] mihomo clone failed, trying pre-built binary download...")
        is_windows = sys.platform == "win32"
        ext = ".exe" if is_windows else ""
        out = BIN_DIR / f"mihomo{ext}"
        BIN_DIR.mkdir(parents=True, exist_ok=True)
        import platform as _pf
        arch = "amd64" if _pf.machine() in ("x86_64", "amd64") else "arm64"
        urls = [
            f"https://github.com/MetaCubeX/mihomo/releases/download/v1.18.1/mihomo-linux-{arch}-v1.18.1.gz",
            f"https://hub.0z.gs/MetaCubeX/mihomo/releases/download/v1.18.1/mihomo-linux-{arch}-v1.18.1.gz",
        ]
        import tempfile, gzip
        for url in urls:
            try:
                tmpdir = Path(tempfile.mkdtemp())
                archive = tmpdir / "mihomo.gz"
                run(["wget", "-q", "-O", str(archive), url])
                with gzip.open(archive, "rb") as f_in:
                    with open(out, "wb") as f_out:
                        shutil.copyfileobj(f_in, f_out)
                os.chmod(str(out), 0o755)
                print(f"[bootstrap] downloaded mihomo binary to {out}")
                return
            except Exception as e:
                print(f"[bootstrap] download from {url} failed: {e}")
        raise RuntimeError("Failed to get mihomo from all sources")
    env = os.environ.copy()
    env["CGO_ENABLED"] = "0"
    env["GOPROXY"] = "https://goproxy.cn,direct"
    is_windows = sys.platform == "win32"
    ext = ".exe" if is_windows else ""
    out = BIN_DIR / f"mihomo{ext}"
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    run(["go", "build", "-trimpath", "-tags", "with_gvisor,with_quic,with_utls", "-ldflags", "-s -w",
         "-o", str(out), "."], cwd=repo, env=env)
    print(f"[bootstrap] built {out}")


def build_tor():
    is_windows = sys.platform == "win32"
    ext = ".exe" if is_windows else ""
    out = BIN_DIR / f"tor{ext}"
    BIN_DIR.mkdir(parents=True, exist_ok=True)

    # Try to install tor from package manager first
    if not is_windows:
        try:
            run(["apk", "add", "--no-cache", "tor"])
            tor_path = shutil.which("tor")
            if tor_path:
                shutil.copyfile(tor_path, str(out))
                os.chmod(str(out), 0o755)
                print(f"[bootstrap] installed tor from apk to {out}")
                return
        except Exception:
            pass

    # Fallback: build from source
    import tempfile
    import tarfile
    tmpdir = Path(tempfile.mkdtemp())
    archive = tmpdir / "tor.tar.gz"
    run(["wget", "-q", "-O", str(archive), TOR_URL])
    with tarfile.open(archive, "r:gz") as tar:
        tar.extractall(tmpdir)
    src_dir = next(tmpdir.glob("tor-*"))
    run(["./configure", "--disable-asciidoc", f"--prefix={tmpdir / 'install'}"], cwd=src_dir)
    run(["make", "-j4"], cwd=src_dir)
    run(["make", "install"], cwd=src_dir)
    built = tmpdir / "install" / "bin" / "tor"
    shutil.copyfile(str(built), str(out))
    os.chmod(str(out), 0o755)
    print(f"[bootstrap] built {out} from source")


def main():
    if shutil.which("git") is None:
        print("[bootstrap] ERROR: git not found", file=sys.stderr)
        return 2
    if shutil.which("go") is None:
        print("[bootstrap] ERROR: go not found", file=sys.stderr)
        return 2

    try:
        build_v2ray()
    except Exception as e:
        print(f"[bootstrap] WARNING: v2ray build failed: {e}")
    try:
        build_singbox()
    except Exception as e:
        print(f"[bootstrap] WARNING: sing-box build failed: {e}")
    try:
        build_mihomo()
    except Exception as e:
        print(f"[bootstrap] WARNING: mihomo build failed: {e}")
    try:
        build_tor()
    except Exception as e:
        print(f"[bootstrap] WARNING: tor build failed: {e}")
    print("[bootstrap] core engines are ready in bin/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
