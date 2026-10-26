#!/usr/bin/env python3
"""Exercise the real Verilated GPU through host/SDRAM/video pins, against SRR."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / ("build/nano-gpu-rtl.exe" if os.name == "nt" else "build/nano-gpu-rtl"))
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rtl-gpu-tests")
    args = parser.parse_args()
    executable = args.executable.resolve()
    build = args.build_dir.resolve(); build.mkdir(parents=True, exist_ok=True)
    result_path = build / "results.json"
    result_path.unlink(missing_ok=True)
    binary_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    results = []
    cases = [("commands", ["--self-test"])]
    cases += [(scene, ["--headless", "--verify", "--scene", scene, "--frames", "2", "--time", "1.25",
                       "--stats", str(build / (scene + ".json")), "--output", str(build / (scene + ".ppm")),
                       "--scanout-log", str(build / (scene + "-scanout.csv"))])
              for scene in ("showcase", "perspective", "overdraw", "opaque")]
    for name, options in cases:
        print("RTL integration: " + name, flush=True)
        log = build / (name + ".log")
        with log.open("w", encoding="utf-8") as output:
            completed = subprocess.run([str(executable)] + options, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        if completed.returncode:
            raise RuntimeError(name + " failed:\n" + log.read_text(errors="replace")[-8000:])
        case = {"name": name, "passed": True, "log": str(log)}
        if name != "commands":
            stats = json.loads((build / (name + ".json")).read_text())
            if stats.get("backend") != "verilator" or stats.get("verified") is not True or stats.get("cpp_device_backends_linked") is not False:
                raise RuntimeError("unexpected simulator/evidence in " + name)
            if stats.get("checked_scanout_frames", 0) < 2 or stats.get("scanout_underflows") != 0:
                raise RuntimeError("incomplete continuous-frame verification or scanout underflow in " + name)
            case["statistics"] = stats
        results.append(case)
    if hashlib.sha256(executable.read_bytes()).hexdigest() != binary_hash:
        raise RuntimeError("simulator binary changed during regression; rebuild and rerun")
    report = {"schema": "N20K-RTL-GPU-TEST-1", "scope": "Functional logical GPU; host bus, physical SDRAM commands and logical scanout",
              "whole_gpu_fit_qualified": False, "simulation_passed": True,
              "executable_sha256": binary_hash, "cases": results}
    result_path.write_text(json.dumps(report, indent=2) + "\n")
    print("All five RTL integration cases passed; " + str(result_path))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print("rtl-gpu-check: " + str(error), file=sys.stderr)
        sys.exit(1)
