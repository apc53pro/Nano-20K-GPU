#!/usr/bin/env python3
"""Compare native raster RTL with the canonical SRR and check fragment II1.

Run with Python 3, a C++17 compiler and Icarus Verilog. Tool discovery follows
rtl-check.py, including NANO_GPU_RTL_TOOLS and tools available on PATH. These
are renderer unit tests; texture-cache, SDRAM, full-device fit and clock timing
require their separate integration and qualification checks.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HDL = runpy.run_path(str(ROOT / "scripts/rtl-check.py"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rtl-renderer")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    report_path = build / "results.json"
    # Failure must not leave a passing report from an earlier source revision.
    report_path.unlink(missing_ok=True)

    rtl = [ROOT / name for name in (
        "rtl/gpu/nano_sdp16.sv", "rtl/uv_mult.v", "rtl/uv_pipeline.v",
        "rtl/gpu/nano_fragment_pipeline.sv", "rtl/gpu/nano_tile_renderer.sv",
        "rtl/gpu/nano_sdp32.sv", "rtl/gpu/nano_sdp36.sv")]
    tests = [ROOT / "rtl/tests" / name for name in (
        "generate_tile_vectors.cpp", "tile_renderer_tb.sv",
        "tile_renderer_fault_tb.sv", "fragment_throughput_tb.sv",
        "tile_quantize_tb.sv", "sdp32_tb.sv", "tile_coefficient_throughput_tb.sv",
        "tile_record_compact_tb.sv")]
    oracle = [ROOT / "reference/src" / name for name in (
        "setup.cpp", "renderer.cpp", "hw_protocol.cpp", "hw_walker2d.cpp")]
    inputs = rtl + tests + oracle + [ROOT / "scripts" / name for name in (
        "rtl-renderer-check.py", "rtl-check.py", "rtl-generate-rom.py")]
    inputs += sorted((ROOT / "reference/include").rglob("*.hpp"))
    inputs += sorted((ROOT / "rtl/generated").glob("reciprocal_seed_*.hex"))
    hashes = {str(path.relative_to(ROOT)): digest(path) for path in inputs}

    seed = build / "seed_rom.v"
    HDL["run"]([sys.executable, str(ROOT / "scripts/rtl-generate-rom.py"), str(seed)],
               build / "rom-generation.log")
    generator = build / ("generate-tile-vectors.exe" if os.name == "nt" else "generate-tile-vectors")
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    # ORACLE_ONLY excludes every C++ GPU hardware-block implementation from
    # this executable; the expected pixels come from RenderMode::Monolithic.
    HDL["run"](compiler + ["-std=c++17", "-O2", "-Wall", "-Wextra",
        "-DNANO_GPU_ORACLE_ONLY", "-I" + str(ROOT / "reference/include"),
        str(tests[0])] + list(map(str, oracle)) + ["-o", str(generator)],
        build / "oracle-compile.log")
    vectors = build / "tile-vectors.txt"
    print(HDL["run"]([str(generator), str(vectors)], build / "oracle-generation.log").strip())
    sources = [rtl[0], rtl[1], seed] + rtl[2:]
    results = []
    for top, testbench, arguments in (
        ("nano_tile_testbench", tests[1], ["+vectors=" + str(vectors)]),
        ("nano_tile_fault_tb", tests[2], []),
        ("nano_fragment_throughput_tb", tests[3], []),
        ("nano_tile_quantize_tb", tests[4], []),
        ("nano_sdp32_tb", tests[5], []),
        ("nano_tile_coefficient_throughput_tb", tests[6], []),
        ("nano_tile_record_compact_tb", tests[7], []),
    ):
        binary = build / (top + ".vvp")
        HDL["run"](HDL["tool"]("iverilog") + ["-g2012", "-s", top, "-o", str(binary)] +
                   list(map(str, sources)) + [str(testbench)], build / (top + "-compile.log"))
        output = HDL["run"](HDL["tool"]("vvp") + [str(binary)] + arguments,
                            build / (top + ".log"))
        if top == "nano_tile_testbench":
            match = re.search(r"(\d+)/(\d+) tile oracle cases passed", output)
            if not match or match[1] != match[2] or int(match[1]) < 25:
                raise RuntimeError("canonical tile test did not report all cases passing")
            result = {"top": top, "passed": True, "tile_cases": int(match[1]),
                      "attachment_samples_compared": 1024 * int(match[1])}
            print(match[0], flush=True)
        elif top == "nano_tile_fault_tb":
            match = re.search(r"PASS (\d+) malformed/numeric records", output)
            if not match or int(match[1]) < 28:
                raise RuntimeError("fault test did not report all records passing")
            result = {"top": top, "passed": True, "rejected_records": int(match[1]),
                      "fault_write_suppression": True, "reset_preserves_attachments": True}
            print(f"{match[1]} malformed/numeric, fault suppression and reset cases passed", flush=True)
        elif top == "nano_fragment_throughput_tb":
            matches = re.findall(r"PASS\s+(sprite|triangle):1024 consecutive accepts and commits, elapsed(\d+)", output)
            if {name for name, _ in matches} != {"sprite", "triangle"}:
                raise RuntimeError("fragment test did not prove both II1 cases")
            result = {"top": top, "passed": True,
                      "fragment_streams": [{"kind": name, "accepted_consecutively": 1024,
                          "retired_consecutively": 1024, "elapsed_edges": int(edges)}
                          for name, edges in matches]}
            for name, edges in matches:
                print(f"{name}: 1024 consecutive accepts and commits in {edges} edges", flush=True)
        elif top == "nano_tile_quantize_tb":
            match = re.search(r"PASS (\d+) full-width/RNE quantizer vectors", output)
            if not match or int(match[1]) < 1000000:
                raise RuntimeError("narrow quantizer equivalence cases did not all pass")
            result = {"top": top, "passed": True,
                      "quantizer_vectors": int(match[1]),
                      "converters": ["RGB8", "D24"],
                      "reference": "Full-width signed saturation and ties-to-even rounding"}
            print(match[0], flush=True)
        elif top == "nano_sdp32_tb":
            if "PASS native SDPB512x32 timing, enables, endpoints and reset retention" not in output:
                raise RuntimeError("native 32-bit coefficient RAM contract test failed")
            result = {"top": top, "passed": True, "native_mode": "SDPB512x32",
                      "independent_clocks": True, "reset_preserves_memory": True}
            print("Native SDPB512x32 timing, enables and reset cases passed", flush=True)
        elif top == "nano_tile_coefficient_throughput_tb":
            matches = re.findall(r"PASS tile\s+(sprite|triangle|minseed):1024 consecutive accepts and commits, elapsed(\d+)", output)
            if {name for name, _ in matches} != {"sprite", "triangle", "minseed"}:
                raise RuntimeError("tile coefficient lookahead did not prove all II1/seed cases")
            result = {"top": top, "passed": True,
                      "coefficient_streams": [{"kind": name, "accepted_consecutively": 1024,
                          "retired_consecutively": 1024, "elapsed_edges": int(edges)}
                          for name, edges in matches], "reverse_command_loading": True,
                      "last_seed_write_with_command_launch": True}
            for name, edges in matches:
                print(f"Tile {name}: 1024 consecutive accepts and commits in {edges} edges", flush=True)
        else:
            match = re.search(r"PASS (\d+) raw32/compact record validation and clipping vectors", output)
            if not match or int(match[1]) < 100000:
                raise RuntimeError("compact header did not preserve full-width record acceptance")
            result = {"top": top, "passed": True, "raw_record_vectors": int(match[1]),
                      "full_width_acceptance_reference": True, "arbitrary_word_order": True,
                      "error_sentinel_rewrites": True, "viewport_intersections": True}
            print(match[0], flush=True)
        results.append(result)

    if any(digest(path) != hashes[str(path.relative_to(ROOT))] for path in inputs):
        raise RuntimeError("renderer test inputs changed during the run; rerun against one source revision")
    report = {"schema": "N20K-RENDERER-RTL-TESTS-1",
              "scope": "Native renderer unit regressions; not whole-GPU timing or fit proof",
              "oracle": "Canonical SRR RenderMode::Monolithic; software hardware-block renderer excluded",
              "texture_boundary": "Ready/valid test responders; cache and SDRAM require separate integration tests",
              "tests": results, "source_sha256": hashes,
              "generated_sha256": {path.name: digest(path) for path in (seed, vectors)}}
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Renderer RTL results: " + str(report_path))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("rtl-renderer-check: " + str(error), file=sys.stderr)
        sys.exit(1)
