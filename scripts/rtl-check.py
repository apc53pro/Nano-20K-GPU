#!/usr/bin/env python3
"""Simulate, synthesize and optionally route the exact shared-reciprocal core."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SUITE = Path(os.environ.get("NANO_GPU_RTL_TOOLS", ROOT / ".deps/rtl/oss-cad-suite")).resolve()


def tool(name):
    extension = ".exe" if os.name == "nt" else ""
    local = SUITE / "bin" / (name + extension)
    if local.is_file():
        if name == "nextpnr-himbaechel" and sys.platform.startswith("linux"):
            # The upstream GUI launcher rewrites XDG directories under HOME.
            # Invoke its bundled loader directly; no HOME override is needed.
            return [str(SUITE / "lib/ld-linux-x86-64.so.2"), "--inhibit-cache",
                    "--inhibit-rpath", "", "--library-path", str(SUITE / "lib"),
                    str(SUITE / "libexec" / name)]
        return [str(local)]
    installed = shutil.which(name)
    if installed:
        return [installed]
    raise RuntimeError(f"missing {name}; run ./scripts/setup-rtl-tools.sh or add your toolchain to PATH")


def environment():
    result = os.environ.copy()
    if SUITE.is_dir():
        search = [SUITE / "bin"]
        # Windows suite helpers load their DLLs from lib, as in the suite's
        # own environment.ps1. No system-wide PATH changes are needed.
        if os.name == "nt": search.append(SUITE / "lib")
        result["PATH"] = os.pathsep.join(map(str, search)) + os.pathsep + result.get("PATH", "")
    return result


def run(command, logfile=None, extra_env=None):
    env = environment()
    if extra_env:
        env.update(extra_env)
    if logfile:
        with logfile.open("w") as output:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT)
        if result.returncode:
            print(logfile.read_text()[-6000:], file=sys.stderr)
            raise RuntimeError(f"tool failed; see {logfile}")
        return logfile.read_text()
    return subprocess.check_output(command, cwd=ROOT, env=env, text=True, stderr=subprocess.STDOUT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--synth", action="store_true")
    parser.add_argument("--pnr", action="store_true")
    parser.add_argument("--require-timing", action="store_true")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rtl")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    inputs = [ROOT / name for name in (
        "rtl/uv_mult.v", "rtl/uv_pipeline.v", "rtl/uv_probe.v", "rtl/uv_probe.cst",
        "rtl/tests/uv_pipeline_tb.v", "rtl/tests/generate_uv_vectors.cpp",
        "rtl/tests/native_mult_modes_tb.sv",
        "rtl/tests/uv_texture_tb.sv", "rtl/tests/generate_texture_vectors.cpp",
        "rtl/gpu/nano_sdp32.sv", "rtl/gpu/nano_sdp36.sv",
        "reference/tests/uv_oracle.hpp", "reference/src/hw_reciprocal.cpp",
        "reference/include/nano_gpu/hw/reciprocal.hpp",
        "reference/include/nano_gpu/hw/reciprocal_seed.hpp",
        "scripts/generate-reciprocal-rom.py", "scripts/rtl-generate-rom.py",
        "scripts/rtl-check.py")]
    inputs += sorted((ROOT / "rtl/generated").glob("reciprocal_seed_*.hex"))
    source_hashes = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in inputs}
    run([sys.executable, str(ROOT / "scripts/generate-reciprocal-rom.py"), "--check"])
    run([sys.executable, str(ROOT / "scripts/rtl-generate-rom.py"), str(build / "seed_rom.v")])
    generator = build / ("generate-vectors.exe" if os.name == "nt" else "generate-vectors")
    compiler = shlex.split(os.environ.get("CXX", "g++"))
    run(compiler + ["-std=c++17", "-O2", str(ROOT / "rtl/tests/generate_uv_vectors.cpp"), "-o", str(generator)])
    print(run([str(generator), str(build / "uv-vectors.txt")]).strip())
    sources = [ROOT / "rtl/uv_mult.v", build / "seed_rom.v", ROOT / "rtl/uv_pipeline.v"]
    run(tool("iverilog") + ["-g2012", "-s", "uv_pipeline_tb", "-o", str(build / "uv.vvp")] +
        list(map(str, sources)) + [str(ROOT / "rtl/tests/uv_pipeline_tb.v")], build / "uv-compile.log")
    print(run(tool("vvp") + [str(build / "uv.vvp"), f"+vectors={build / 'uv-vectors.txt'}"],
              build / "uv-simulation.log").strip())
    run(tool("iverilog") + ["-g2012", "-s", "native_mult_modes_tb", "-o",
        str(build / "native-mult-modes.vvp"), str(ROOT / "rtl/uv_mult.v"),
        str(ROOT / "rtl/tests/native_mult_modes_tb.sv")], build / "native-mult-modes-compile.log")
    multiplier_output = run(tool("vvp") + [str(build / "native-mult-modes.vvp")],
                            build / "native-mult-modes.log")
    if "PASS native multiplier: all four sign modes" not in multiplier_output:
        raise RuntimeError("native multiplier modes did not pass their complete checks")
    print(multiplier_output.strip())
    texture_generator = build / ("generate-texture-vectors.exe" if os.name == "nt" else "generate-texture-vectors")
    run(compiler + ["-std=c++17", "-O2", str(ROOT / "rtl/tests/generate_texture_vectors.cpp"),
        "-o", str(texture_generator)])
    print(run([str(texture_generator), str(build / "uv-vectors.txt"),
        str(build / "texture-vectors.txt")]).strip())
    texture_sources = sources + [ROOT / "rtl/gpu/nano_sdp32.sv", ROOT / "rtl/gpu/nano_sdp36.sv"]
    run(tool("iverilog") + ["-g2012", "-s", "uv_texture_tb", "-o", str(build / "texture.vvp")] +
        list(map(str, texture_sources)) + [str(ROOT / "rtl/tests/uv_texture_tb.sv")],
        build / "texture-compile.log")
    print(run(tool("vvp") + [str(build / "texture.vvp"),
        f"+vectors={build / 'texture-vectors.txt'}"], build / "texture-simulation.log").strip())
    report = {"schema": "N20K-UV-RTL-0.1", "device": "GW2AR-LV18QN88C8/I7",
              "scope": "shared reciprocal/UV core in serial qualification harness",
              "family": "GW2A-18C", "enabled_edge_latency": 26, "initiation_interval": 1,
              "simulation_passed": True, "whole_gpu_validated": False,
              "target_mhz": 100.8, "synthesized": False, "routed": False,
              "selected_uv_limb_bits": 18,
              "selected_native_multiplier_modes": ["unsigned_unsigned", "unsigned_signed"],
              "native_multiplier_modes_test_passed": True,
              "texture_coordinate_tests": {"passed": True, "enabled_edge_latency": 19,
                  "initiation_interval": 1, "exact_high16_and_overflow_of_full_q16": True,
                  "retention_bsram_blocks": 8},
              "modeled_seed_bsram_blocks": 4,
              "probe_stimulus_bits": 198, "probe_checksum_bits": 32,
              "toolchain_release": "2026-10-24" if SUITE.is_dir() else "user-selected PATH",
              "source_sha256": source_hashes,
              "generated_sha256": {
                  path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in (build / "seed_rom.v", build / "uv-vectors.txt", build / "texture-vectors.txt")}}
    if args.synth or args.pnr:
        print("Synthesizing the arithmetic qualification harness...", flush=True)
        files = " ".join(json.dumps(str(path)) for path in sources + [ROOT / "rtl/uv_probe.v"])
        command = f"read_verilog -sv -D SYNTHESIS {files}; synth_gowin -family gw2a -top nano_uv_probe -json {json.dumps(str(build / 'uv-synth.json'))}; stat"
        report["yosys_version"] = run(tool("yosys") + ["-V"]).strip()
        run(tool("yosys") + ["-Q", "-T", "-p", command], build / "uv-synthesis.log")
        design = json.loads((build / "uv-synth.json").read_text())
        counts = Counter(cell["type"] for cell in design["modules"]["nano_uv_probe"]["cells"].values())
        report.update(synthesized=True, synthesis_cells=dict(sorted(counts.items())))
        print(json.dumps({"synthesis_cells": report["synthesis_cells"]}, indent=2))
    if args.pnr:
        print("Routing the exact QN88 part; this does not program the board...", flush=True)
        command = tool("nextpnr-himbaechel") + ["--device", report["device"], "-o", "family=GW2A-18C",
            "-o", "cst=" + str(ROOT / "rtl/uv_probe.cst"), "--json", str(build / "uv-synth.json"),
            "--write", str(build / "uv-routed.json"), "--freq", "100.8", "--timing-allow-fail",
            "--report", str(build / "uv-timing.json"), "--detailed-timing-report"]
        database = SUITE / "share/nextpnr/himbaechel/gowin/chipdb-GW2A-18C.bin"
        if database.is_file():
            command += ["--chipdb", str(database)]
        extra = {"PYTHONHOME": str(SUITE)} if SUITE.is_dir() else {}
        report["nextpnr_version"] = run(tool("nextpnr-himbaechel") + ["--version"], extra_env=extra).strip()
        run(command, build / "uv-pnr.log", extra_env=extra)
        timing = json.loads((build / "uv-timing.json").read_text())
        report.update(routed=True, timing=timing)
        fmax = timing.get("fmax", {})
        report["target_met"] = bool(fmax) and all(value["achieved"] >= value["constraint"] for value in fmax.values())
        print(json.dumps({"fmax": fmax, "target_met": report["target_met"]}, indent=2))
    if any(hashlib.sha256(path.read_bytes()).hexdigest() != source_hashes[str(path.relative_to(ROOT))]
           for path in inputs):
        raise RuntimeError("qualification inputs changed during the run; rerun against one source revision")
    (build / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Qualification report: {build / 'qualification.json'}")
    if args.require_timing and not report.get("target_met", False):
        raise RuntimeError("100.8-MHz timing target was not met; see the actual timing report")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"rtl-check: {error}", file=sys.stderr)
        sys.exit(1)
