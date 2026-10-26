#!/usr/bin/env python3
"""Build the GPU RTL as a Verilator C++/SDL simulator and synthesize it."""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("nano_rtl_tools", ROOT / "scripts/rtl-check.py")
HDL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HDL)


def run(command, logfile=None):
    if logfile:
        logfile.parent.mkdir(parents=True, exist_ok=True)
        with logfile.open("w", encoding="utf-8") as output:
            result = subprocess.run(command, cwd=ROOT, env=HDL.environment(), stdout=output, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError("command failed; see {}\n{}".format(logfile, logfile.read_text(errors="replace")[-10000:]))
        return ""
    return subprocess.check_output(command, cwd=ROOT, env=HDL.environment(), text=True, stderr=subprocess.STDOUT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rtl-gpu")
    parser.add_argument("--output", type=Path, default=ROOT / ("build/nano-gpu-rtl.exe" if os.name == "nt" else "build/nano-gpu-rtl"))
    parser.add_argument("--headless-only", action="store_true")
    parser.add_argument("--synth", action="store_true")
    parser.add_argument("--run", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    build = args.build_dir.resolve(); build.mkdir(parents=True, exist_ok=True)
    report_path = build / "build-report.json"
    report_path.unlink(missing_ok=True)
    output = args.output.resolve(); output.parent.mkdir(parents=True, exist_ok=True)
    run([sys.executable, str(ROOT / "scripts/rtl-generate-rom.py"), str(build / "seed_rom.v")])
    rtl = [ROOT / "rtl/uv_mult.v", build / "seed_rom.v", ROOT / "rtl/uv_pipeline.v"]
    rtl += sorted((ROOT / "rtl/gpu").glob("*.sv"))
    cpp = [ROOT / "reference/src" / (name + ".cpp") for name in ("setup", "renderer", "demo", "hw_protocol")]
    cpp += [ROOT / "reference/rtl/main.cpp", ROOT / "reference/rtl/sdram_model.cpp"]
    inputs = rtl + cpp + sorted((ROOT / "reference/rtl").glob("*.hpp"))
    inputs += sorted((ROOT / "reference/include").rglob("*.hpp"))
    inputs += sorted((ROOT / "rtl/generated").glob("*.hex"))
    inputs += [ROOT / "scripts/gpu-rtl.py", ROOT / "scripts/rtl-generate-rom.py", ROOT / "scripts/rtl-check.py"]
    fingerprints = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    cflags = ["-std=c++17", "-O2", "-DNANO_GPU_ORACLE_ONLY=1", "-I" + str(ROOT / "reference/include"),
              "-I" + str(ROOT / "reference/rtl")]
    ldflags = []
    sdl = False
    if not args.headless_only:
        env = HDL.environment()
        local = ROOT / ".deps/sdl2/lib/pkgconfig"
        if local.is_dir(): env["PKG_CONFIG_PATH"] = str(local) + os.pathsep + env.get("PKG_CONFIG_PATH", "")
        try:
            cflags += shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl2"], env=env, text=True, stderr=subprocess.DEVNULL))
            ldflags += shlex.split(subprocess.check_output(["pkg-config", "--libs", "sdl2"], env=env, text=True))
            if os.name != "nt":
                libdir = subprocess.check_output(["pkg-config", "--variable=libdir", "sdl2"], env=env, text=True).strip()
                if libdir: ldflags += ["-Wl,-rpath," + libdir]
            cflags += ["-DNANO_GPU_RTL_SDL=1"]; sdl = True
        except (OSError, subprocess.CalledProcessError):
            print("gpu-rtl: SDL2 unavailable; producing a headless RTL simulator", flush=True)
    command = HDL.tool("verilator") + ["--cc", "--exe", "--build", "-j", "4", "--trace", "--assert",
        "--top-module", "nano_gpu_core", "--prefix", "Vnano_gpu_core", "--Mdir", str(build / "obj"),
        "-Wno-fatal", "-CFLAGS", shlex.join(cflags), "-o", str(output)]
    if ldflags: command += ["-LDFLAGS", shlex.join(ldflags)]
    command += list(map(str, rtl + cpp))
    print("Building Verilator GPU from functional RTL...", flush=True)
    run(command, build / "verilator-build.log")
    report = {
        "schema": "N20K-GPU-RTL-BUILD-1", "top": "nano_gpu_core",
        "device": "GW2AR-LV18QN88C8/I7", "family": "GW2A-18C",
        "simulation_backend": "verilator", "sdl_enabled": sdl,
        "cpp_device_backends_linked": False,
        "scope": "Functional command-to-pixel core; production clocking, TMDS wrapper and board pins are not integrated",
        "whole_gpu_fit_qualified": False,
        "verilator_version": run(HDL.tool("verilator") + ["--version"]).strip(),
        "executable_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "cpp_translation_units": [str(path.relative_to(ROOT)) for path in cpp],
        "source_sha256": fingerprints,
    }
    if args.synth:
        netlist = build / "gpu-synth.json"
        sources = " ".join(json.dumps(str(path)) for path in rtl)
        yosys = "read_verilog -sv -D SYNTHESIS {}; synth_gowin -family gw2a -top nano_gpu_core -json {}; stat".format(sources, json.dumps(str(netlist)))
        print("Synthesizing the same functional core for Gowin...", flush=True)
        run(HDL.tool("yosys") + ["-Q", "-T", "-p", yosys], build / "gpu-synthesis.log")
        design = json.loads(netlist.read_text())
        counts = Counter(cell["type"] for cell in design["modules"]["nano_gpu_core"]["cells"].values())
        report["synthesis_cells"] = dict(sorted(counts.items()))
        report["yosys_version"] = run(HDL.tool("yosys") + ["-V"]).strip()
        report["netlist_sha256"] = hashlib.sha256(netlist.read_bytes()).hexdigest()
        print(json.dumps({"core_synthesis_cells": report["synthesis_cells"]}, indent=2))
    changed = [str(path.relative_to(ROOT)) for path in inputs if hashlib.sha256(path.read_bytes()).hexdigest() != fingerprints[str(path.relative_to(ROOT))]]
    if changed: raise RuntimeError("RTL/build inputs changed during the run; rebuild: " + ", ".join(changed))
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("RTL simulator: " + str(output))
    if args.run is not None: subprocess.run([str(output)] + args.run, cwd=ROOT, check=True)


if __name__ == "__main__":
    try: main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("gpu-rtl: " + str(error), file=sys.stderr); sys.exit(1)
