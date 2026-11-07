#!/usr/bin/env python3
"""A/B diagnostic: build the unchanged tang20k_top RTL with the vendor Gowin EDA.

This is not the qualified board flow (scripts/build-tang20k.py). It answers one
question: does the same RTL behave on the board when Gowin's own synthesis,
place-and-route and bitstream generator are used instead of the open flow?
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
DEVICE = "GW2AR-LV18QN88C8/I7"
FAMILY = "GW2AR-18C"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gowin", default=r"C:\Gowin\Gowin_V1.9.8.11_Education\IDE\bin\gw_sh.exe")
    args = ap.parse_args()
    build = ROOT / "build/gowin-ab"
    src = build / "src"
    src.mkdir(parents=True, exist_ok=True)
    rom = src / "seed_rom.v"
    subprocess.run([sys.executable, str(ROOT / "scripts/rtl-generate-rom.py"), str(rom)], cwd=ROOT, check=True)
    sources = [ROOT / "rtl/uv_mult.v", ROOT / "rtl/uv_pipeline.v"] + sorted((ROOT / "rtl/gpu").glob("*.sv")) + \
              [ROOT / "rtl/board/tang20k_top.sv"]
    # The open flow passes -D SYNTHESIS; give Gowin the same view per file.
    copies = [rom]
    rom.write_text("`define SYNTHESIS\n" + rom.read_text(), newline="\n")
    # Syntax-only rewrites for GowinSynthesis, applied to the build copies; the
    # repository RTL stays byte-identical. Each must match exactly once.
    rewrites = {
        "nano_fragment_pipeline.sv": [(
            "data4={8'b0,factor_data[23:16]},data5;",
            "data4={8'b0,factor_data[23:16]};\n    wire [15:0] data5;")],
    }
    for path in sources:
        text = path.read_text()
        for old, new in rewrites.get(path.name, []):
            if text.count(old) != 1: sys.exit(f"gowin-ab: rewrite not unique in {path.name}: {old}")
            text = text.replace(old, new)
        copy = src / path.name
        copy.write_text("`define SYNTHESIS\n" + text, newline="\n")
        copies.append(copy)
    board_cst = ROOT / "rtl/board/tang20k.cst"
    # Gowin's PnR derives each differential N pin from its P pin and rejects a
    # second constraint on hdmi_n. Drop only those lines; the pin report is
    # checked against the original file afterwards.
    cst = build / "tang20k-gowin.cst"
    cst.write_text("\n".join(l for l in board_cst.read_text().splitlines() if '"hdmi_n[' not in l) + "\n", newline="\n")
    sdc = build / "tang20k.sdc"
    # PLL/CLKDIV outputs are derived by Gowin. The 6.3 MHz core clock is the
    # fabric divider core_phase[1] (pixel clock / 4); constrain it explicitly.
    # The host clock is asynchronous; the RTL crosses it with synchronizers.
    sdc.write_text("create_clock -name clk27 -period 37.037 [get_ports {clk27}]\n"
                   "create_clock -name gpu_hclk -period 100000 [get_ports {gpu_hclk}]\n"
                   "create_clock -name core_clk -period 158.730 [get_pins {core_phase_1_s0/Q}]\n"
                   "set_clock_groups -asynchronous -group [get_clocks {gpu_hclk}]"
                   " -group [get_clocks {core_clk}]\n", newline="\n")
    tcl = build / "build.tcl"
    lines = [f"set_device -name {FAMILY} {DEVICE}"]
    lines += [f"add_file {{{p.as_posix()}}}" for p in copies]
    lines += [f"add_file {{{cst.as_posix()}}}", f"add_file {{{sdc.as_posix()}}}",
              "set_option -top_module tang20k_top",
              "set_option -verilog_std sysv2017",
              "set_option -output_base_name nano-gpu-gowin",
              "set_option -use_mspi_as_gpio 1",
              "set_option -use_sspi_as_gpio 1",
              "set_option -use_ready_as_gpio 1",
              "set_option -use_done_as_gpio 1",
              "set_option -use_reconfign_as_gpio 1",
              "set_option -timing_driven 1",
              "set_option -gen_text_timing_rpt 1",
              "run all"]
    tcl.write_text("\n".join(lines) + "\n", newline="\n")
    report = {"schema": "N20K-GOWIN-AB-1", "purpose": "Vendor-toolchain A/B diagnostic of unchanged tang20k_top RTL",
              "qualified_board_image": False, "hardware_tested": False, "gowin_shell": args.gowin,
              "source_sha256": {p.relative_to(ROOT).as_posix(): digest(p) for p in sources + [board_cst]}}
    log = build / "gw_sh.log"
    with log.open("w", encoding="utf-8", errors="replace") as out:
        result = subprocess.run([args.gowin, str(tcl)], cwd=build, stdout=out, stderr=subprocess.STDOUT)
    report["gw_sh_returncode"] = result.returncode
    images = sorted(build.rglob("nano-gpu-gowin.fs"))
    if images:
        report["bitstream"] = images[0].relative_to(ROOT).as_posix()
        report["bitstream_sha256"] = digest(images[0])
    (build / "report.json").write_text(json.dumps(report, indent=2) + "\n", newline="\n")
    print(log.read_text(errors="replace")[-4000:])
    if result.returncode or not images:
        sys.exit("gowin-ab: build failed; see " + str(log))
    print("Gowin A/B image: " + str(images[0]) + "\nSHA-256: " + report["bitstream_sha256"])


if __name__ == "__main__":
    main()
