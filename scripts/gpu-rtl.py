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
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("nano_rtl_tools", ROOT / "scripts/rtl-check.py")
HDL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HDL)


VERBOSE = False


def run(command, logfile=None):
    if logfile:
        logfile.parent.mkdir(parents=True, exist_ok=True)
        print("Log: " + str(logfile), flush=True)
        with logfile.open("w", encoding="utf-8") as output:
            started = time.monotonic()
            if VERBOSE:
                process = subprocess.Popen(command, cwd=ROOT, env=HDL.environment(), stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
                with process.stdout as stream:
                    for line in stream:
                        output.write(line); print(line, end="", flush=True)
                status = process.wait()
            else:
                process = subprocess.Popen(command, cwd=ROOT, env=HDL.environment(), stdout=output, stderr=subprocess.STDOUT)
                while True:
                    try:
                        status = process.wait(timeout=5)
                        break
                    except subprocess.TimeoutExpired:
                        print("Still building ({:.0f}s); details in {}".format(time.monotonic() - started, logfile), flush=True)
        if status:
            raise RuntimeError("command failed; see {}\n{}".format(logfile, logfile.read_text(errors="replace")[-10000:]))
        return ""
    return subprocess.check_output(command, cwd=ROOT, env=HDL.environment(), text=True, stderr=subprocess.STDOUT)


def verilator_command(windows=None):
    """Native Python cannot execute MSYS2's extensionless Perl launcher."""
    if windows is None: windows = os.name == "nt"
    if not windows: return HDL.tool("verilator")
    env = HDL.environment()
    directories = [HDL.SUITE / "bin"] + [Path(p.strip('"')) for p in env.get("PATH", "").split(os.pathsep) if p]
    for directory in directories:
        executable = directory / "verilator.exe"
        if executable.is_file(): return [executable.as_posix()]
        launcher = directory / "verilator"
        if not launcher.is_file(): continue
        cygpath = shutil.which("cygpath", path=env.get("PATH"))
        if not cygpath:
            raise RuntimeError("MSYS2's Verilator launcher requires MSYS2 UCRT64 and Perl; run ./scripts/setup-rtl-sim.sh")
        perl = subprocess.check_output([cygpath, "-w", "/usr/bin/perl.exe"], env=env, text=True).strip()
        if not Path(perl).is_file():
            raise RuntimeError("MSYS2 Perl is missing; run ./scripts/setup-rtl-sim.sh")
        script = subprocess.check_output([cygpath, "-u", launcher.as_posix()], env=env, text=True).strip()
        return [Path(perl).as_posix(), script]
    raise RuntimeError("Verilator is missing; run ./scripts/setup-rtl-sim.sh from MSYS2 UCRT64")


def preflight(build, headless_only=False, require_sdl=False):
    print("Checking RTL simulator dependencies...", flush=True)
    print("Python: " + sys.executable, flush=True)
    env = HDL.environment()
    if not shutil.which("make", path=env.get("PATH")):
        raise RuntimeError("make is missing; run ./scripts/setup-rtl-sim.sh")
    compiler = shlex.split(os.environ.get("CXX", "g++"))
    try:
        print("C++: " + run(compiler + ["--version"]).splitlines()[0], flush=True)
        verilator = verilator_command()
        print(run(verilator + ["--version"]).strip(), flush=True)
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        details = str(error)
        if isinstance(error, subprocess.CalledProcessError) and error.output:
            details += "\n" + str(error.output)[-4000:]
        raise RuntimeError("Cannot run the RTL toolchain; run ./scripts/setup-rtl-sim.sh\n" + details) from error
    # We advance VerilatedContext explicitly. MinGW does not provide the weak
    # legacy sc_time_stamp hook that ELF builds can leave undefined.
    flags = ["-std=c++17", "-O2", "-DVL_TIME_CONTEXT=1", "-DNANO_GPU_ORACLE_ONLY=1", "-I" + (ROOT / "reference/include").as_posix(),
             "-I" + (ROOT / "reference/rtl").as_posix()]
    links = []
    sdl = False
    if not headless_only:
        local = ROOT / ".deps/sdl2/lib/pkgconfig"
        if local.is_dir(): env["PKG_CONFIG_PATH"] = str(local) + os.pathsep + env.get("PKG_CONFIG_PATH", "")
        try:
            sdl_flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "sdl2"], env=env, text=True, stderr=subprocess.DEVNULL))
            sdl_links = shlex.split(subprocess.check_output(["pkg-config", "--libs", "sdl2"], env=env, text=True))
            if os.name != "nt":
                libdir = subprocess.check_output(["pkg-config", "--variable=libdir", "sdl2"], env=env, text=True).strip()
                if libdir: sdl_links += ["-Wl,-rpath," + libdir]
            flags += sdl_flags + ["-DNANO_GPU_RTL_SDL=1"]; links += sdl_links; sdl = True
        except (OSError, subprocess.CalledProcessError):
            if require_sdl: raise RuntimeError("SDL2 development files are missing; run ./scripts/setup-rtl-sim.sh")
            print("SDL2 unavailable: building a headless simulator. Use ./scripts/setup-rtl-sim.sh for a window.", flush=True)
    print("SDL2: " + ("enabled" if sdl else "disabled (headless)"), flush=True)
    return verilator, flags, links, sdl


def smoke_check(build, verilator, flags, links, sdl):
    """Compile/run actual RTL with the same C++/SDL link flags as the GPU."""
    check = build / "toolcheck"; check.mkdir(parents=True, exist_ok=True)
    source = check / "tools_smoke.sv"
    source.write_text("module nano_tools_smoke(input wire clk, input wire a, output reg y); always @(posedge clk) y <= a; endmodule\n")
    cpp = check / "tools_smoke.cpp"
    cpp.write_text('#include "Vnano_tools_smoke.h"\n'
                   '#ifdef NANO_GPU_RTL_SDL\n#include <SDL.h>\n#endif\n'
                   'static_assert(sizeof(__int128) == 16, "compiler must support __int128");\n'
                   'int main(int argc, char** argv) { (void)argc; (void)argv;\n#ifdef NANO_GPU_RTL_SDL\nSDL_SetMainReady(); if (SDL_Init(0)) return 3; SDL_Quit();\n#endif\n'
                   'Vnano_tools_smoke dut; dut.clk=0; dut.a=1; dut.eval(); dut.clk=1; dut.eval(); return dut.y != 1; }\n')
    executable = check / ("tools-smoke.exe" if os.name == "nt" else "tools-smoke")
    command = verilator + ["--cc", "--exe", "--build", "-j", "2", "-Wno-fatal", "--top-module", "nano_tools_smoke",
                           "--Mdir", (check / "obj").as_posix(), "-CFLAGS", shlex.join(flags), "-o", executable.as_posix()]
    if links: command += ["-LDFLAGS", shlex.join(links)]
    command += [source.as_posix(), cpp.as_posix()]
    print("Compiling a small RTL/C++" + ("/SDL2" if sdl else "") + " smoke test...", flush=True)
    run(command, check / "build.log")
    run([executable.as_posix()])
    print("RTL simulator toolchain check passed.", flush=True)


def main():
    global VERBOSE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rtl-gpu")
    parser.add_argument("--output", type=Path, default=ROOT / ("build/nano-gpu-rtl.exe" if os.name == "nt" else "build/nano-gpu-rtl"))
    parser.add_argument("--headless-only", action="store_true")
    parser.add_argument("--require-sdl", action="store_true", help="fail instead of silently building without a window")
    parser.add_argument("--check", action="store_true", help="compile/run a small RTL/C++/SDL toolchain test")
    parser.add_argument("--verbose", action="store_true", help="show compiler output as it is produced")
    parser.add_argument("--synth", action="store_true")
    parser.add_argument("--board-core", action="store_true", help="Simulate the Tang board core profile: 50.4 MHz core, 25.2 MHz pixel/host ticks")
    parser.add_argument("--run", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.headless_only and args.require_sdl: parser.error("--headless-only and --require-sdl cannot be combined")
    VERBOSE = args.verbose
    build = args.build_dir.resolve(); build.mkdir(parents=True, exist_ok=True)
    verilator, cflags, ldflags, sdl = preflight(build, args.headless_only, args.require_sdl)
    if args.board_core:
        cflags += ["-DNANO_GPU_BOARD_PROFILE=1"]
    if args.check:
        smoke_check(build, verilator, cflags, ldflags, sdl)
        return
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
    fingerprints = {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    command = verilator + ["--cc", "--exe", "--build", "-j", "4", "--trace", "--assert",
        "--top-module", "nano_gpu_core", "--prefix", "Vnano_gpu_core", "--Mdir", (build / "obj").as_posix(),
        "-Wno-fatal", "-CFLAGS", shlex.join(cflags), "-o", output.as_posix()]
    if ldflags: command += ["-LDFLAGS", shlex.join(ldflags)]
    if args.board_core:
        command += ["-GCORE_HZ=50400000", "-GPIXEL_DIVIDER=2", "-GEXTERNAL_PIXEL_TICK=1",
                    "-GREFRESH_INTERVAL=787", "-GHOST_DIVIDER_METADATA=0"]
    command += [path.as_posix() for path in rtl + cpp]
    print("Building Verilator GPU from functional RTL...", flush=True)
    run(command, build / "verilator-build.log")
    report = {
        "schema": "N20K-GPU-RTL-BUILD-1", "top": "nano_gpu_core",
        "device": "GW2AR-LV18QN88C8/I7", "family": "GW2A-18C",
        "simulation_backend": "verilator", "sdl_enabled": sdl,
        "board_core_profile": args.board_core,
        "board_core_profile_scope": "50.4 MHz core/SDRAM with external 25.2 MHz pixel ticks and a 25.2 MHz virtual host; TMDS serializers and board pins are outside this core simulation" if args.board_core else None,
        "cpp_device_backends_linked": False,
        "scope": "Functional command-to-pixel core; production clocking, TMDS wrapper and board pins are not integrated",
        "whole_gpu_fit_qualified": False,
        "verilator_version": run(verilator + ["--version"]).strip(),
        "executable_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "cpp_translation_units": [str(path.relative_to(ROOT)) for path in cpp],
        "source_sha256": fingerprints,
    }
    if args.synth:
        netlist = build / "gpu-synth.json"
        sources = " ".join(json.dumps(str(path)) for path in rtl)
        yosys = "read_verilog -sv -D SYNTHESIS {}; synth_gowin -family gw2a -top nano_gpu_core -json {} -nowidelut; stat".format(sources, json.dumps(str(netlist)))
        report["synthesis_options"] = ["-nowidelut"]
        print("Synthesizing the same functional core for Gowin...", flush=True)
        run(HDL.tool("yosys") + ["-Q", "-T", "-p", yosys], build / "gpu-synthesis.log")
        design = json.loads(netlist.read_text())
        counts = Counter(cell["type"] for cell in design["modules"]["nano_gpu_core"]["cells"].values())
        report["synthesis_cells"] = dict(sorted(counts.items()))
        report["yosys_version"] = run(HDL.tool("yosys") + ["-V"]).strip()
        report["netlist_sha256"] = hashlib.sha256(netlist.read_bytes()).hexdigest()
        print(json.dumps({"core_synthesis_cells": report["synthesis_cells"]}, indent=2))
    changed = [path.relative_to(ROOT).as_posix() for path in inputs if hashlib.sha256(path.read_bytes()).hexdigest() != fingerprints[path.relative_to(ROOT).as_posix()]]
    if changed: raise RuntimeError("RTL/build inputs changed during the run; rebuild: " + ", ".join(changed))
    report_path.write_text(json.dumps(report, indent=2) + "\n", newline="\n")
    print("RTL simulator: " + str(output))
    if args.run is not None: subprocess.run([str(output)] + args.run, cwd=ROOT, check=True)


if __name__ == "__main__":
    try: main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("gpu-rtl: " + str(error), file=sys.stderr); sys.exit(1)
