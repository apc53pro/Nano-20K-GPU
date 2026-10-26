#!/usr/bin/env python3
"""Install a pinned, workspace-local OSS CAD Suite; never changes system files."""
import argparse
import hashlib
import os
from pathlib import Path
import platform
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
RELEASE = "2026-10-24"
ARCHIVE = "oss-cad-suite-linux-x64-20261024.tgz"
SHA256 = "f33dc250de5e3d2b4325c32dafc695c44e4f19cdc1e3bdb1fea7c78a7c5daa0a"
URL = f"https://github.com/YosysHQ/oss-cad-suite-build/releases/download/{RELEASE}/{ARCHIVE}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    directory = ROOT / ".deps/rtl"
    suite = directory / "oss-cad-suite"
    required = [suite / "bin" / name for name in ("iverilog", "vvp", "verilator", "yosys", "nextpnr-himbaechel")]
    if all(path.is_file() for path in required):
        print(f"rtl-setup: toolchain available at {suite}")
        return
    if args.check:
        raise SystemExit("rtl-setup: local toolchain is missing; run without --check")
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "AMD64"):
        raise SystemExit("Automatic bootstrap supports Linux x86_64. Install OSS CAD Suite for your platform and add its bin directory to PATH; see docs/hardware/rtl-qualification.md.")
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / ARCHIVE
    if not archive.exists():
        print(f"rtl-setup: downloading pinned {RELEASE} suite (approximately 749 MB)", flush=True)
        temporary = archive.with_suffix(".part")
        with urllib.request.urlopen(URL, timeout=120) as source, temporary.open("wb") as output:
            while block := source.read(1024 * 1024):
                output.write(block)
        temporary.replace(archive)
    digest = hashlib.sha256()
    with archive.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    if digest.hexdigest() != SHA256:
        raise SystemExit("rtl-setup: downloaded archive failed SHA256 verification")
    print("rtl-setup: extracting verified tools", flush=True)
    with tarfile.open(archive, "r:gz") as source:
        # Reject traversal and links outside the workspace extraction directory.
        source.extractall(directory, filter="data")
    if not all(path.is_file() for path in required):
        raise SystemExit("rtl-setup: archive did not contain the required tools")
    print(f"rtl-setup: installed {suite}")


if __name__ == "__main__":
    main()
