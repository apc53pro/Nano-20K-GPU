#!/usr/bin/env python3
"""Check an experimental lossless upload decoder and measure standalone cycles.

No production packet, SDRAM path, or GPU frequency is changed. The paced source
is synthetic: 25.2 MHz offered bytes at an assumed 68.88 MHz decoder clock.
"""
import argparse
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("codec_hdl", ROOT / "scripts/rtl-check.py")
HDL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HDL)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/upload-codec-measurement")
    parser.add_argument("--report", type=Path, help="Optional versioned evidence destination")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    vectors = build / "traffic"
    build.mkdir(parents=True, exist_ok=True)
    experiment = ROOT / "experiments/upload-codec"
    cpp_sources = [experiment / "traffic.cpp"] + [ROOT / "reference/src" / name for name in
        ("setup.cpp", "renderer.cpp", "demo.cpp", "hw_protocol.cpp")]
    inputs = cpp_sources + [experiment / name for name in
        ("nano_upload_word_decoder.sv", "nano_upload_word_decoder_tb.sv", "traffic_tb.sv")]
    inputs += sorted((ROOT / "reference/include").rglob("*.hpp"))
    inputs += [Path(__file__).resolve(), ROOT / "scripts/rtl-check.py"]
    source_hashes = {p.relative_to(ROOT).as_posix(): digest(p) for p in inputs}
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    generator = build / ("traffic-generator.exe" if os.name == "nt" else "traffic-generator")
    HDL.run(compiler + ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Wshadow",
        "-DNANO_GPU_ORACLE_ONLY=1", "-I" + str(ROOT / "reference/include")] +
        list(map(str, cpp_sources)) + ["-o", str(generator)], build / "traffic-generator-build.log")
    print(HDL.run([str(generator), str(vectors)], build / "traffic-generator.log").strip(), flush=True)
    traffic = json.loads((vectors / "traffic.json").read_text())
    common_args = ["+encoded=" + str(vectors / "encoded.hex"),
        "+expected=" + str(vectors / "expected.hex"), "+lengths=" + str(vectors / "lengths.hex"),
        "+records=" + str(traffic["vector_records"])]
    unit = build / "decoder-unit.vvp"
    HDL.run(HDL.tool("iverilog") + ["-g2012", "-s", "nano_upload_word_decoder_tb", "-o", str(unit),
        str(experiment / "nano_upload_word_decoder.sv"), str(experiment / "nano_upload_word_decoder_tb.sv")],
        build / "decoder-unit-compile.log")
    unit_output = HDL.run(HDL.tool("vvp") + [str(unit)] + common_args, build / "decoder-unit.log")
    match = re.search(r"PASS upload word decoder records=(\d+) words=(\d+) encoded_bytes=(\d+) cycles=(\d+) checks=(\d+)", unit_output)
    if not match:
        raise RuntimeError("Decoder unit tests did not complete")
    unit_summary = dict(zip(("records", "words", "encoded_bytes", "cycles", "checks"), map(int, match.groups())))
    print(match.group(0), flush=True)
    workload = build / "traffic.vvp"
    HDL.run(HDL.tool("iverilog") + ["-g2012", "-s", "upload_codec_traffic_tb", "-o", str(workload),
        str(experiment / "nano_upload_word_decoder.sv"), str(experiment / "traffic_tb.sv")], build / "traffic-compile.log")
    with (vectors / "records.csv").open(newline="") as source:
        metadata = list(csv.DictReader(source))
    if len(metadata) != traffic["vector_records"] or any(int(r["record"]) != i for i, r in enumerate(metadata)):
        raise RuntimeError("Workload metadata is not contiguous")
    simulations = []
    for mode, sink_period in ((0, 1), (1, 1), (1, 4)):
        csv_path = build / f"cycles-{mode}-{sink_period}.csv"
        output = HDL.run(HDL.tool("vvp") + [str(workload)] + common_args +
            [f"+bytes={traffic['vector_encoded_bytes']}", f"+mode={mode}", f"+sink_period={sink_period}",
             "+csv=" + str(csv_path)], build / f"cycles-{mode}-{sink_period}.log")
        if "PASS workload " not in output:
            raise RuntimeError("Standalone workload simulation did not complete")
        print(output.splitlines()[0], flush=True)
        with csv_path.open(newline="") as source:
            rows = list(csv.DictReader(source))
        if len(rows) != len(metadata):
            raise RuntimeError("Workload omitted records")
        cases = {}
        for i, (row, meta) in enumerate(zip(rows, metadata)):
            if int(row["record"]) != i or int(row["encoded_bytes"]) != int(meta["encoded_bytes"]) or int(row["expanded_bytes"]) != 320:
                raise RuntimeError("Workload and exact encoder metadata disagree")
            key = (meta["scene"], int(meta["frame"]))
            case = cases.setdefault(key, {"scene": key[0], "frame": key[1], "records": 0,
                "core_cycles": 0, "encoded_bytes": 0, "expanded_bytes": 0,
                "input_stall_cycles": 0, "triangle_records": 0, "triangle_core_cycles": 0})
            for name in ("core_cycles", "encoded_bytes", "expanded_bytes", "input_stall_cycles"):
                case[name] += int(row[name])
            case["records"] += 1
            if int(meta["kind"]) == 0:
                case["triangle_records"] += 1
                case["triangle_core_cycles"] += int(row["core_cycles"])
        for case in cases.values():
            if mode == 0 and case["core_cycles"] != case["encoded_bytes"] + 80 * case["records"]:
                raise RuntimeError("No-stall throughput formula failed")
            case["standalone_microseconds_at_assumed_68_88_mhz"] = case["core_cycles"] / 68.88
            case["triangle_microseconds_at_assumed_68_88_mhz"] = case["triangle_core_cycles"] / 68.88
        simulations.append({"source": "always valid" if mode == 0 else "synthetic paced source, phase increment15/modulus41, one pending byte",
            "source_mode": mode, "sink_acceptance_period_core_cycles": sink_period,
            "assumed_core_mhz": 68.88, "assumed_host_mhz": 25.2 if mode else None,
            "source_phase_restarts_each_record": True,
            "record_start_handshake_cycles_excluded": 1,
            "scope": "Decoder only: no packet parser, CDC FIFO, staging RAM, SDRAM stores, CPU or frame rendering",
            "all_records_bit_exact": True, "cases": list(cases.values()),
            "cycle_csv_sha256": digest(csv_path)})
    changed = [name for name, expected in source_hashes.items() if digest(ROOT / name) != expected]
    if changed:
        raise RuntimeError("Measurement inputs changed: " + ", ".join(changed))
    report = {"schema": "N20K-UPLOAD-CODEC-CHECK-1", "production_protocol_changed": False,
        "integrated_upload_throughput_measured": False, "frame_fps_measured": False,
        "real_hardware_measured": False, "source_sha256": source_hashes,
        "inputs_changed_during_run": changed, "unit_tests": unit_summary,
        "traffic": traffic, "standalone_simulations": simulations,
        "vector_sha256": {name: digest(vectors / name) for name in
            ("encoded.hex", "expected.hex", "lengths.hex", "records.csv", "traffic.json")}}
    write_json(build / "checks.json", report)
    if args.report:
        write_json(args.report.resolve(), report)
    print("Standalone cycle and correctness evidence: " + str(build / "checks.json"))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print("check-upload-codec: " + str(error), file=sys.stderr)
        sys.exit(1)
