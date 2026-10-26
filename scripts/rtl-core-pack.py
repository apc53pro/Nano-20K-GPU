#!/usr/bin/env python3
"""Synthesize and pack the complete logical GPU in a serial resource probe.

This preserves the core's external input/output cones with nine physical pins.
It deliberately does not claim a board pinout, routed fit, clock closure, or
the resources of a production PLL/TMDS wrapper.
"""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("nano_pack_tools", ROOT / "scripts/rtl-check.py")
HDL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HDL)


def run(command, logfile, extra_env=None):
    env = HDL.environment()
    if extra_env:
        env.update(extra_env)
    with logfile.open("w", encoding="utf-8") as output:
        return subprocess.run(command, cwd=ROOT, env=env, stdout=output,
                              stderr=subprocess.STDOUT).returncode


def physical_lut_lower_bound(cells, capacity=20736):
    """Include FF input LUTs omitted by the Gowin pack-only inventory.

    In the selected nextpnr Gowin architecture a colocated FF must consume
    that LUT's F or ALU's SUM. Other FFs need a free LUT, populated with a
    passthrough after placement. Ignore control-set/cluster restrictions here:
    this is an optimistic lower bound, never a placement or routing proof.
    See nextpnr 22c5adf4, gowin.cc slice_valid/create_passthrough_luts.
    """
    drivers = {}
    for name, cell in cells.items():
        for port, direction in cell.get("port_directions", {}).items():
            if direction == "output":
                for bit in cell["connections"][port]:
                    drivers[bit] = (name, cell["type"], port)
    ff_inputs = Counter()
    for cell in cells.values():
        if cell["type"].startswith("DFF"):
            ff_inputs[cell["connections"]["D"][0]] += 1
    pairable = 0
    for bit in ff_inputs:
        _, kind, port = drivers.get(bit, (None, None, None))
        if ((kind in ("LUT1", "LUT2", "LUT3", "LUT4") and port == "F") or
                (kind == "ALU" and port == "SUM")):
            pairable += 1
    active_ff = sum(ff_inputs.values())
    feed_luts = active_ff - pairable
    occupied = sum(cell["type"] in ("LUT1", "LUT2", "LUT3", "LUT4", "BLOCKER_LUT")
                   for cell in cells.values())
    return {"scope": "Optimistic FF-input LUT lower bound for the selected nextpnr Gowin mapping; not legal placement or timing",
            "active_flipflops": active_ff,
            "maximum_existing_lut_alu_pairs": pairable,
            "minimum_additional_ff_input_luts": feed_luts,
            "packed_lut_sites": occupied,
            "minimum_physical_lut_sites": occupied + feed_luts,
            "available_lut_sites": capacity,
            "within_capacity": occupied + feed_luts <= capacity}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rtl-core-pack")
    parser.add_argument("--report", type=Path, default=ROOT / "docs/hardware/rtl-core-pack.json",
                        help="compact versioned report, written even when capacity is rejected")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    report_path = build / "qualification.json"
    report_path.unlink(missing_ok=True)
    rom = build / "seed_rom.v"
    subprocess.run([sys.executable, str(ROOT / "scripts/rtl-generate-rom.py"), str(rom)],
                   cwd=ROOT, env=HDL.environment(), check=True)
    sources = [ROOT / "rtl/uv_mult.v", rom, ROOT / "rtl/uv_pipeline.v"]
    sources += sorted((ROOT / "rtl/gpu").glob("*.sv"))
    sources += [ROOT / "rtl/gpu_probe.sv"]
    inputs = [path for path in sources if path != rom]
    inputs += [ROOT / "rtl/gpu_probe.cst", Path(__file__).resolve(),
               ROOT / "scripts/rtl-generate-rom.py", ROOT / "scripts/rtl-check.py"]
    inputs += sorted((ROOT / "rtl/generated").glob("*.hex"))
    hashes = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in inputs}
    report = {
        "schema": "N20K-CORE-PACK-PROBE-1", "top": "nano_gpu_probe",
        "device": "GW2AR-LV18QN88C8/I7", "family": "GW2A-18C",
        "scope": "Complete functional logical GPU in a serial qualification probe, including probe overhead",
        "whole_gpu_fit_qualified": False, "board_pinout_qualified": False,
        "production_pll_tmds_integrated": False, "routed": False,
        "timing_qualified": False, "synthesized": False, "packed": False,
        "synthesis_options": ["-nowidelut"],
        "probe_input_shift_bits": 50, "probe_output_snapshot_bits": 116,
        "probe_checksum_bits": 32, "probe_maximum_register_overhead": 198,
        "source_sha256": hashes,
        "generated_rom_sha256": hashlib.sha256(rom.read_bytes()).hexdigest(),
    }
    netlist = build / "gpu-probe-synth.json"
    elaboration_status = run(HDL.tool("iverilog") + ["-g2012", "-s", "nano_gpu_probe", "-o",
        str(build / "probe-elaboration.vvp")] + list(map(str, sources)),
        build / "probe-elaboration.log")
    report["elaboration_passed"] = elaboration_status == 0
    if elaboration_status:
        raise RuntimeError("probe elaboration failed; see " + str(build / "probe-elaboration.log"))
    files = " ".join(json.dumps(str(path)) for path in sources)
    # LUT4-only mapping avoids wide-MUX placement clusters and permits more
    # register inputs to share the LUT computing their data on this device.
    script = "read_verilog -sv -D SYNTHESIS {}; synth_gowin -family gw2a -top nano_gpu_probe -json {} -nowidelut; stat".format(
        files, json.dumps(str(netlist)))
    print("Synthesizing the complete logical GPU serial probe...", flush=True)
    synth_log = build / "synthesis.log"
    synth_status = run(HDL.tool("yosys") + ["-Q", "-T", "-p", script], synth_log)
    report["synthesis_returncode"] = synth_status
    if synth_status == 0:
        design = json.loads(netlist.read_text())
        counts = Counter(cell["type"] for cell in design["modules"]["nano_gpu_probe"]["cells"].values())
        report.update(synthesized=True, synthesis_cells=dict(sorted(counts.items())),
                      netlist_sha256=hashlib.sha256(netlist.read_bytes()).hexdigest())
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"synthesis_cells": report["synthesis_cells"]}, indent=2), flush=True)
        packed_path = build / "gpu-probe-packed.json"
        packed_path.unlink(missing_ok=True)
        pack_log = build / "pack.log"
        extra = {"PYTHONHOME": str(HDL.SUITE)} if HDL.SUITE.is_dir() else {}
        command = HDL.tool("nextpnr-himbaechel") + [
            "--device", report["device"], "-o", "family=" + report["family"],
            "-o", "cst=" + str(ROOT / "rtl/gpu_probe.cst"),
            "--json", str(netlist), "--write", str(packed_path), "--pack-only"]
        database = HDL.SUITE / "share/nextpnr/himbaechel/gowin/chipdb-GW2A-18C.bin"
        if database.is_file():
            command += ["--chipdb", str(database)]
        print("Packing the exact QN88 part; no placement or routing...", flush=True)
        pack_status = run(command, pack_log, extra)
        report["pack_returncode"] = pack_status
        report["packed"] = pack_status == 0
        text = pack_log.read_text(errors="replace")
        report["packer_errors"] = re.findall(r"^ERROR:\s*(.*)$", text, re.MULTILINE)
        utilization = {}
        for name, used, available in re.findall(r"^Info:\s+([A-Za-z0-9_]+):\s+(\d+)\s*/\s*(\d+)", text, re.MULTILINE):
            utilization[name] = {"used": int(used), "available": int(available),
                                 "within_capacity": int(used) <= int(available)}
        report["packer_utilization"] = utilization
        if packed_path.is_file():
            packed = json.loads(packed_path.read_text())
            # nextpnr serializes its selected design as module "top" rather
            # than retaining Yosys's original top-module name.
            top = packed["modules"]["top"]
            packed_counts = Counter(cell["type"] for cell in top["cells"].values())
            report["packed_cells"] = dict(sorted(packed_counts.items()))
            report["packed_netlist_sha256"] = hashlib.sha256(packed_path.read_bytes()).hexdigest()
            report["physical_lut_lower_bound"] = physical_lut_lower_bound(top["cells"])
            print(json.dumps({"physical_lut_lower_bound": report["physical_lut_lower_bound"]}, indent=2), flush=True)
        report["reported_resources_within_capacity"] = bool(utilization) and all(
            item["within_capacity"] for item in utilization.values())
        report["capacity_rejected_resources"] = [
            name for name, item in utilization.items() if not item["within_capacity"]]
        print(text[-12000:], flush=True)
    else:
        report["synthesis_error_tail"] = synth_log.read_text(errors="replace")[-8000:]
        print(report["synthesis_error_tail"], file=sys.stderr)
    changed = [str(path.relative_to(ROOT)) for path in inputs
               if hashlib.sha256(path.read_bytes()).hexdigest() != hashes[str(path.relative_to(ROOT))]]
    report["inputs_changed_during_run"] = changed
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("Serial probe qualification report: " + str(report_path))
    if changed:
        raise RuntimeError("qualification sources changed during the run: " + ", ".join(changed))
    # Keep the source revision's actual capacity result without committing
    # generated netlists or verbose tool logs. Cell-type inventories remain in
    # the build report; native resource occupancy is the compact public result.
    compact = {name: value for name, value in report.items()
               if name not in ("synthesis_cells", "packed_cells")}
    args.report.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.report.resolve().write_text(json.dumps(compact, indent=2) + "\n", encoding="utf-8")
    print("Versioned resource result: " + str(args.report.resolve()))
    if not report["packed"]:
        return 1
    if (not report.get("reported_resources_within_capacity", False) or
            not report.get("physical_lut_lower_bound", {}).get("within_capacity", False)):
        return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("rtl-core-pack: " + str(error), file=sys.stderr)
        sys.exit(1)
