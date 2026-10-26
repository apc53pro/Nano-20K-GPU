#!/usr/bin/env python3
"""Check native RAM/video RTL, including deliberate collision rejection."""

import argparse
import json
from pathlib import Path
import runpy
import subprocess


ROOT = Path(__file__).resolve().parents[1]
HDL = runpy.run_path(str(ROOT / "scripts/rtl-check.py"))


def invoke(command, logfile):
    result = subprocess.run(command, cwd=ROOT, env=HDL["environment"](),
                            capture_output=True, text=True)
    logfile.write_text(result.stdout + result.stderr, encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rtl-native")
    parser.add_argument("--host", action="store_true", help="also rerun asynchronous host transport tests")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    report_path = build / "results.json"
    # A failed rerun must not leave an older passing report in this directory.
    report_path.unlink(missing_ok=True)
    sources = [ROOT / name for name in ("rtl/gpu/nano_sdp16.sv", "rtl/gpu/nano_scanout.sv",
                                      "rtl/gpu/nano_tmds.sv", "rtl/tests/native_video_tb.sv")]
    cases = [(name, [], False) for name in
             ("sdp16_tb", "sdp16_async_tb", "tmds_encoder_tb", "native_video_tb", "scanout_contention_tb")]
    cases.append(("sdp16_collision_tb", [], True))
    if args.host:
        for half_period, phase in ((13, 3), (5, 0), (3, 0)):
            cases.append(("nano_host_link_tb",
                          ["-Pnano_host_link_tb.HOST_HALF_PERIOD=" + str(half_period),
                           "-Pnano_host_link_tb.HOST_PHASE=" + str(phase)], False))
    results = []
    for index, (top, parameters, expect_collision) in enumerate(cases):
        label = top + "-" + str(index)
        binary = build / (label + ".vvp")
        files = sources
        if top == "nano_host_link_tb":
            files = [ROOT / name for name in ("rtl/gpu/nano_sdp16.sv", "rtl/gpu/nano_host_link.sv",
                                               "rtl/tests/nano_host_link_tb.sv")]
        compilation = invoke(HDL["tool"]("iverilog") + ["-g2012", "-s", top, "-o", str(binary)] +
                             parameters + [str(path) for path in files], build / (label + "-compile.log"))
        if compilation.returncode:
            raise SystemExit("Native RTL compile failed: " + compilation.stderr)
        run = invoke(HDL["tool"]("vvp") + [str(binary)], build / (label + ".log"))
        collision_detected = "nano_sdp16 undefined same-address read/write collision" in (run.stdout + run.stderr)
        passed = run.returncode != 0 and collision_detected if expect_collision else run.returncode == 0
        if not passed:
            raise SystemExit("Native RTL test failed: " + label + "\n" + run.stdout + run.stderr)
        results.append({"top": top, "parameters": parameters, "passed": True,
                        "expected_collision_rejected": expect_collision})
        print("PASS " + label + (" (undefined collision rejected)" if expect_collision else ""), flush=True)
    report = {"schema": "N20K-NATIVE-RTL-TESTS-1", "scope": "Native primitive and video unit tests; not full-GPU fit proof",
              "tests": results}
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Native RTL results: " + str(report_path))


if __name__ == "__main__":
    main()
