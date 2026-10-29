#!/usr/bin/env python3
"""Check core recovery and registered primitive-record delivery."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HDL = runpy.run_path(str(ROOT / "scripts/rtl-check.py"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rtl-core-recovery")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    report = build / "results.json"
    report.unlink(missing_ok=True)
    sources = sorted((ROOT / "rtl/gpu").glob("*.sv")) + [ROOT / name for name in (
        "rtl/uv_mult.v", "rtl/uv_pipeline.v", "rtl/tests/core_recovery_quiesce_tb.sv")]
    inputs = sources + [ROOT / "scripts/rtl-core-recovery-check.py", ROOT / "scripts/rtl-check.py",
                        ROOT / "scripts/rtl-generate-rom.py"] + sorted((ROOT / "rtl/generated").glob("reciprocal_seed_*.hex"))
    hashes = {str(path.relative_to(ROOT)): digest(path) for path in inputs}
    rom = build / "seed_rom.v"
    HDL["run"]([sys.executable, str(ROOT / "scripts/rtl-generate-rom.py"), str(rom)], build / "rom.log")
    binary = build / "recovery"
    HDL["run"](HDL["tool"]("verilator") + ["--binary", "--timing", "-Wno-fatal", "--top-module", "nano_core_recovery_quiesce_tb",
               "--Mdir", str(build / "obj"), "--build-jobs", "2", "-o", str(binary)] +
               [str(path) for path in sources] + [str(rom)], build / "compile.log")
    output = HDL["run"]([str(binary)], build / "simulation.log")
    required = ["PASS cache acceptance on abort quiesce entry drains before reset",
                "PASS cache acceptance on fault quiesce entry drains before reset",
                "PASS accepted upload payload and physical WRITE drain while quiet",
                "PASS active renderer keeps texture fetches enabled until abort quiescence",
                "PASS eighty record words retain one-edge latency, burst/gap order and final-word-before-launch",
                *("PASS queued record cancellation kind%d preserves later draw state" % kind for kind in range(3))]
    if any(line not in output for line in required):
        raise RuntimeError("Recovery test did not establish every required race/drain case")
    summary = re.search(r"PASS (\d+) recovery checks:", output)
    if not summary or int(summary[1]) < 1000:
        raise RuntimeError("Core recovery/record test did not complete its assertion checks")
    if any(digest(ROOT / name) != value for name, value in hashes.items()):
        raise RuntimeError("Recovery test inputs changed during the run")
    report.write_text(json.dumps({"schema": "N20K-RTL-RECOVERY-1", "passed": True,
        "scope": "Complete core; four recovery cases use the actual SDRAM service with a controlled cache producer and boot fixture; four record cases control the read-response boundary",
        "simulation_backend": "verilator", "case_count": len(required), "assertion_checks": int(summary[1]),
        "cases": required, "record_word_pipeline": {"words": 80, "latency_edges": 1,
            "maximum_accepted_words_per_edge": 1, "sustained_first_burst_words": 16,
            "later_response_bubbles_checked": True, "last_word_before_launch_checked": True,
            "cancellation_checked": ["abort", "fault", "reset"], "fresh_epoch_record_checked": True},
        "source_sha256": hashes, "generated_rom_sha256": digest(rom),
        "executable_sha256": digest(binary)}, indent=2) + "\n")
    print(output, end="")
    print("Recovery evidence: " + str(report))


if __name__ == "__main__":
    main()
