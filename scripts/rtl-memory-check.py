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
    arbiter_test = ROOT / "rtl/tests/sdram_arbiter_tb.sv"
    accept_check_test = ROOT / "rtl/tests/sdram_accept_check_tb.sv"
    texture_rtl = [ROOT / name for name in (
        "rtl/uv_mult.v", "rtl/gpu/nano_sdp16.sv", "rtl/gpu/nano_texture_cache.sv",
        "rtl/tests/texture_cache_tb.sv")]
    inputs = service_rtl + service_cpp + texture_rtl + [arbiter_test, accept_check_test] + [ROOT / name for name in (
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

    arbiter_binary = build / "sdram-arbiter.vvp"
    print("Building and testing exhaustive SDRAM arbitration equivalence...", flush=True)
    checked(HDL["tool"]("iverilog") + ["-g2012", "-s", "sdram_arbiter_tb", "-o", str(arbiter_binary)] +
            list(map(str, service_rtl + [arbiter_test])), "arbiter-build.log")
    arbiter_output = checked(HDL["tool"]("vvp") + [str(arbiter_binary)], "arbiter-test.log")
    match = re.search(r"PASS (\d+) exhaustive SDRAM arbiter grant/ready/metadata cases", arbiter_output)
    if not match or int(match[1]) != 15360:
        raise RuntimeError("Arbiter regression did not complete all cases; see arbiter-test.log")
    arbiter_cases = int(match[1])
    match = re.search(r"PASS (\d+) SDRAM extent boundary/alignment/raw-count cases", arbiter_output)
    if not match or int(match[1]) != 8192:
        raise RuntimeError("SDRAM extent regression did not complete all cases; see arbiter-test.log")
    extent_cases = int(match[1])
    print(arbiter_output.strip(), flush=True)

    accept_check_binary = build / "sdram-accept-check.vvp"
    print("Building and testing retained SDRAM command validation...", flush=True)
    checked(HDL["tool"]("iverilog") + ["-g2012", "-s", "nano_sdram_accept_check_tb", "-o", str(accept_check_binary)] +
            list(map(str, service_rtl + [accept_check_test])), "accept-check-build.log")
    accept_check_output = checked(HDL["tool"]("vvp") + [str(accept_check_binary)], "accept-check-test.log")
    match = re.search(r"PASS (\d+) targeted CHECK request cases, (\d+) assertions, (\d+) edges", accept_check_output)
    if not match or int(match[1]) != 43 or int(match[2]) < 4000:
        raise RuntimeError("Retained-command validation cases did not complete; see accept-check-test.log")
    accept_check_cases, accept_check_assertions, accept_check_edges = map(int, match.groups())
    print(accept_check_output.strip(), flush=True)

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
    match = re.search(r"PASS configuration: (\d+) cancellation/drain/reset/allocation cases, "
                      r"(\d+) accepted configurations", texture_output)
    if not match or int(match[1]) < 16:
        raise RuntimeError("Staged texture configuration cases did not complete; see texture-test.log")
    configuration_cases, configurations = map(int, match.groups())
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
            {"top": "nano_sdram_accept_check_tb", "backend": "iverilog/vvp", "passed": True,
             "cases": accept_check_cases, "assertions": accept_check_assertions, "edges": accept_check_edges,
             "additional_service_startup_cycles": 1,
             "coverage": ["all six command owners and both transfer directions",
                          "withdrawn or mutated live fields cannot alter an accepted request",
                          "invalid retained fields complete once for their captured owner before RAM or WRITE side effects",
                          "refresh suspends and resumes retained command validation",
                          "runtime reset cancels staged commands without a physical WRITE tail",
                          "prior read-response ownership survives backpressure and DRAIN before handoff"],
             "log": "accept-check-test.log"},
            {"top": "sdram_arbiter_tb", "backend": "iverilog/vvp", "passed": True,
             "exhaustive_cases": arbiter_cases,
             "extent_boundary_cases": extent_cases,
             "reference": "Former rotating-index selector, all reachable cursor/request/streak combinations",
             "coverage": ["all 64 request masks and five foreground cursors", "all eight streak bit patterns",
                          "bounded scanout priority and foreground wraparound", "IDLE/initialization/refresh/reset/WRITE-tail eligibility",
                          "selected address/length validation, including malformed commands",
                          "full-width byte-end equivalence at both sides of the final 64-byte line, all alignments/raw counts"],
             "added_arbitration_cycles": 0, "log": "arbiter-test.log"},
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
             "configuration_preparation_cycles": 2, "configuration_cases": configuration_cases,
             "accepted_configurations": configurations,
             "coverage": ["warm-hit acceptance/output interval of one cycle", "stable responses and commands under backpressure",
                          "bounded refill addresses", "repeat and clamp coordinates", "row conflicts and large pitches",
                          "flush invalidation", "invalid texture configuration", "memory-error cancellation and reset recovery",
                          "independent full-width descriptor product/end validation and exact 8-MiB boundary",
                          "withdrawal, flush and reset before configuration acceptance preserve descriptor ownership",
                          "configuration waits for an old held response before preparing and committing"],
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
