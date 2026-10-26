#!/usr/bin/env python3
"""Regress the SDRAM pin controller and texture-cache RTL without SDL or the GPU main."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shlex
import sys


ROOT = Path(__file__).resolve().parents[1]
HDL = runpy.run_path(str(ROOT / "scripts/rtl-check.py"))


def fingerprints(paths):
    return {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(set(paths))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rtl-memory")
    parser.add_argument("-j", "--jobs", type=int, default=2, help="Verilator C++ build jobs (default: 2)")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    report_path = build / "results.json"
    # Never leave an older success report behind after a failed rerun.
    report_path.unlink(missing_ok=True)
    service_rtl = [ROOT / name for name in (
        "rtl/gpu/nano_sdp16.sv", "rtl/gpu/nano_sdram_service.sv")]
    service_cpp = [ROOT / name for name in (
        "rtl/tests/sdram_service_tb.cpp", "reference/rtl/sdram_model.cpp")]
    texture_rtl = [ROOT / name for name in (
        "rtl/uv_mult.v", "rtl/gpu/nano_sdp16.sv", "rtl/gpu/nano_texture_cache.sv",
        "rtl/tests/texture_cache_tb.sv")]
    inputs = service_rtl + service_cpp + texture_rtl + [ROOT / name for name in (
        "reference/rtl/sdram_model.hpp", "scripts/rtl-check.py", "scripts/rtl-memory-check.py")]
    source_hashes = fingerprints(inputs)
    commands = []

    def checked(command, log_name):
        commands.append({"argv": command, "log": log_name})
        return HDL["run"](command, build / log_name)

    versions = {}
    for name, version_flag in (("verilator", "--version"), ("iverilog", "-V"), ("vvp", "-V")):
        version = checked(HDL["tool"](name) + [version_flag], name + "-version.log")
        versions[name] = version.strip().splitlines()[0]
    compiler = shlex.split(os.environ.get("CXX", "g++"))
    compiler_version = checked(compiler + ["--version"], "cxx-version.log")
    versions["cxx"] = compiler_version.strip().splitlines()[0]

    binary = build / ("sdram-service-test.exe" if os.name == "nt" else "sdram-service-test")
    cflags = ["-std=c++17", "-O2", "-I" + str(ROOT / "reference/rtl")]
    command = HDL["tool"]("verilator") + [
        "--cc", "--exe", "--build", "--assert", "-j", str(args.jobs), "-Wno-fatal",
        "--top-module", "nano_sdram_service", "--Mdir", str(build / "sdram-obj"),
        "-CFLAGS", shlex.join(cflags), "-o", str(binary)]
    command += list(map(str, service_rtl + service_cpp))
    print("Building and testing SDRAM physical-pin controller...", flush=True)
    checked(command, "sdram-build.log")
    service_output = checked([str(binary)], "sdram-test.log")
    match = re.search(
        r"SDRAM pin regression passed: (\d+) requests, (\d+) physical reads, (\d+) physical writes, "
        r"max refresh gap (\d+) cycles; all eight runtime WRITE reset phases", service_output)
    if not match:
        raise RuntimeError("SDRAM regression did not emit its completion marker; see sdram-test.log")
    requests, reads, writes, refresh_gap = map(int, match.groups())
    print(service_output.strip(), flush=True)

    texture_binary = build / "texture-cache.vvp"
    command = HDL["tool"]("iverilog") + [
        "-g2012", "-s", "texture_cache_tb", "-o", str(texture_binary)] + list(map(str, texture_rtl))
    print("Building and testing texture cache...", flush=True)
    checked(command, "texture-build.log")
    texture_output = checked(HDL["tool"]("vvp") + [str(texture_binary)], "texture-test.log")
    match = re.search(
        r"PASS: (\d+) accepted \((\d+) returned, (\d+) cancelled\), (\d+) bounded refills, "
        r"1024 consecutive warm outputs", texture_output)
    if not match:
        raise RuntimeError("Texture regression did not emit its completion marker; see texture-test.log")
    accepted, returned, cancelled, refills = map(int, match.groups())
    print(texture_output.strip(), flush=True)

    if fingerprints(inputs) != source_hashes:
        raise RuntimeError("Regression inputs changed while tests ran; rerun against stable sources")
    report = {
        "schema": "N20K-MEMORY-RTL-TESTS-1",
        "completed_utc": datetime.now(timezone.utc).isoformat(),
        "passed": True,
        "scope": "SDRAM physical-pin controller and texture-cache unit regressions; not full-GPU fit or timing proof",
        "whole_gpu_fit_qualified": False,
        "tool_versions": versions,
        "source_sha256": source_hashes,
        "commands": commands,
        "tests": [
            {"top": "nano_sdram_service", "backend": "verilator", "passed": True,
             "external_memory_model": "reference/rtl/sdram_model.cpp; old pins sampled at rising edge, DQ established after edge",
             "requests": requests, "physical_reads": reads, "physical_writes": writes,
             "max_refresh_gap_cycles": refresh_gap, "runtime_write_reset_phases": 8,
             "coverage": ["all eight BL8 prefixes and 1..16-word request lengths", "byte masks and unchanged burst padding",
                          "bank, row and allocation boundaries", "producer/consumer stalls across refresh",
                          "held read responses under backpressure", "invalid commands", "six-client arbitration fairness",
                          "runtime reset during each physical WRITE phase without resetting the external chip"],
             "log": "sdram-test.log"},
            {"top": "texture_cache_tb", "backend": "iverilog/vvp", "passed": True,
             "external_memory_model": "bounded client-response model in rtl/tests/texture_cache_tb.sv",
             "accepted": accepted, "returned": returned, "cancelled": cancelled, "bounded_refills": refills,
             "consecutive_warm_outputs": 1024, "warm_hit_handshake_latency_cycles": 7,
             "coverage": ["warm-hit acceptance/output interval of one cycle", "stable responses and commands under backpressure",
                          "bounded refill addresses", "repeat and clamp coordinates", "row conflicts and large pitches",
                          "flush invalidation", "invalid texture configuration", "memory-error cancellation and reset recovery"],
             "log": "texture-test.log"},
        ],
    }
    temporary = report_path.with_suffix(".tmp")
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(report_path)
    print("Memory RTL results: " + str(report_path), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError) as error:
        raise SystemExit("rtl-memory-check: " + str(error)) from error
