#!/usr/bin/env python3
"""Measure GPU/decoder co-residency; this is not upload-path integration."""
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


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


HDL = module("codec_gpu_hdl", "rtl-check.py")
PACK = module("codec_gpu_pack", "rtl-core-pack.py")
PNR = module("codec_gpu_pnr", "rtl-core-pnr.py")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/upload-codec-measurement/gpu")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--no-route", action="store_true")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    sources = [ROOT / "rtl/uv_mult.v", build / "seed_rom.v", ROOT / "rtl/uv_pipeline.v"]
    sources += sorted((ROOT / "rtl/gpu").glob("*.sv"))
    sources += [ROOT / "experiments/upload-codec/nano_upload_word_decoder.sv",
                ROOT / "experiments/upload-codec/gpu_codec_probe.sv"]
    inputs = [p for p in sources if p != build / "seed_rom.v"]
    inputs += [ROOT / "rtl/gpu_probe.cst", Path(__file__).resolve(),
               ROOT / "scripts/rtl-generate-rom.py", ROOT / "scripts/rtl-check.py",
               ROOT / "scripts/rtl-core-pack.py", ROOT / "scripts/rtl-core-pnr.py",
               ROOT / "docs/hardware/rtl-core-pnr.json"]
    inputs += sorted((ROOT / "rtl/generated").glob("*.hex"))
    hashes = {p.relative_to(ROOT).as_posix(): digest(p) for p in inputs}
    # Source snapshots make retained logs reproducible without changing the
    # frozen production files or trusting a later working-tree revision.
    selected = []
    for source in inputs:
        destination = build / "source_snapshot" / source.relative_to(ROOT)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(source.read_bytes())
    HDL.run([sys.executable, str(ROOT / "scripts/rtl-generate-rom.py"), str(build / "seed_rom.v")],
            build / "rom.log")
    for source in sources:
        selected.append(source if source == build / "seed_rom.v" else build / "source_snapshot" / source.relative_to(ROOT))
    baseline = json.loads((ROOT / "docs/hardware/rtl-core-pnr.json").read_text())
    extra = {"PYTHONHOME": str(HDL.SUITE)} if HDL.SUITE.is_dir() else {}
    report = {"schema": "N20K-UPLOAD-CODEC-GPU-PROBE-1",
              "scope": "Unchanged complete logical GPU plus independently stimulated decoder; co-residency probe only",
              "device": PNR.DEVICE, "family": PNR.FAMILY,
              "compressed_upload_path_integrated": False,
              "production_protocol_changed": False, "whole_gpu_fit_qualified": False,
              "board_pinout_qualified": False, "real_hardware_measured": False,
              "probe_extra_input_bits": 11, "probe_extra_capture_bits": 44,
              "source_sha256": hashes, "seed": 1, "synthesis_options": ["-nowidelut"],
              "clock_targets_mhz": PNR.TARGETS,
              "yosys_version": HDL.run(HDL.tool("yosys") + ["-V"]).strip(),
              "nextpnr_version": HDL.run(HDL.tool("nextpnr-himbaechel") + ["--version"], extra_env=extra).strip(),
              "frozen_baseline_physical_resources": baseline["post_route_physical_resources"],
              "baseline_comparison_scope": "Previous committed baseline, not a new matched-probe A/B run; delta includes additional probe registers and packing/placement changes",
              "synthesized": False, "packed": False, "placed": False, "routed": False,
              "post_route_physical_resources": None,
              "post_route_resources_within_capacity": None,
              "fmax": None, "clock_constraints_verified": False,
              "target_met": None, "timing_qualified": False,
              "timing_scope": "nextpnr static timing model for this co-residency probe, not real hardware or the production GPU",
              "frozen_production_rtl_sources_match": all(sha == baseline["source_sha256"].get(name)
                  for name, sha in hashes.items() if name.startswith("rtl/"))}
    report_path = build / "qualification.json"
    write(report_path, report)
    netlist = build / "synth.json"
    script = "read_verilog -sv -D SYNTHESIS {}; synth_gowin -family gw2a -top nano_gpu_codec_probe -nowidelut -json {}; stat".format(
        " ".join(json.dumps(str(p)) for p in selected), json.dumps(str(netlist)))
    print("Synthesizing unchanged GPU plus experimental decoder...", flush=True)
    HDL.run(HDL.tool("yosys") + ["-Q", "-T", "-p", script], build / "synthesis.log")
    design = json.loads(netlist.read_text())["modules"]["nano_gpu_codec_probe"]
    report.update(synthesized=True, synthesis_cells=dict(sorted(Counter(c["type"] for c in design["cells"].values()).items())),
                  synthesized_netlist_sha256=digest(netlist), generated_rom_sha256=digest(build / "seed_rom.v"))
    write(report_path, report)
    command = HDL.tool("nextpnr-himbaechel") + ["--device", PNR.DEVICE, "-o", "family=" + PNR.FAMILY,
        "-o", "cst=" + str(build / "source_snapshot/rtl/gpu_probe.cst"), "--json", str(netlist),
        "--seed", "1", "--threads", "2"]
    database = HDL.SUITE / "share/nextpnr/himbaechel/gowin/chipdb-GW2A-18C.bin"
    if database.is_file():
        command += ["--chipdb", str(database)]
    packed = build / "packed.json"
    print("Packing co-resident blocks on exact QN88 part...", flush=True)
    HDL.run(command + ["--pack-only", "--write", str(packed)], build / "pack.log", extra)
    cells = json.loads(packed.read_text())["modules"]["top"]["cells"]
    report.update(packed=True, physical_lut_lower_bound=PACK.physical_lut_lower_bound(cells))
    write(report_path, report)
    if not args.no_route:
        clocks = build / "clocks.py"
        clocks.write_text("".join('ctx.addClock({}, {})\n'.format(json.dumps(PNR.CLOCK_NETS[name]), target)
                                  for name, target in PNR.TARGETS.items()))
        routed, timing = build / "routed.json", build / "timing.json"
        full_command = command + ["--pre-place", str(clocks), "--freq", "100.8", "--timing-allow-fail",
            "--report", str(timing), "--detailed-timing-report", "--write", str(routed)]
        print("Routing co-residency probe (core 100.8 / host 25.2 MHz constraints)...", flush=True)
        env = HDL.environment(); env.update(extra)
        result = PNR.run_pnr(full_command, env, build / "pnr.log")
        report["route_returncode"] = result.returncode
        report["command"] = full_command
        text = (build / "pnr.log").read_text(errors="replace")
        verified = {}
        for name, target in PNR.TARGETS.items():
            matches = re.findall(r"^Info:\s+constraining clock net '" + re.escape(PNR.CLOCK_NETS[name]) + r"' to ([0-9.]+) MHz", text, re.MULTILINE)
            verified[name] = bool(matches) and abs(float(matches[-1])-target) < 0.001
        report["clock_constraint_confirmation"] = verified
        report["clock_constraints_verified"] = all(verified.values())
        if result.returncode:
            report["error"] = text[-6000:]
            report["error_stage"] = "placement_and_route"
        else:
            routed_design = json.loads(routed.read_text())["modules"]["top"]
            resources = PNR.physical_resources(routed_design["cells"])
            timing_data = json.loads(timing.read_text())
            report.update(placed=PNR.setting(routed_design.get("settings", {}).get("place")),
                          routed=PNR.setting(routed_design.get("settings", {}).get("route")),
                          post_route_physical_resources=resources, fmax=timing_data.get("fmax", {}),
                          post_route_resources_within_capacity=all(item["within_capacity"] for item in resources.values()),
                          critical_paths=PNR.critical_paths(timing_data),
                          decoder_probe_minus_frozen_baseline_resources={name: resources[name]["used"] - baseline["post_route_physical_resources"][name]["used"] for name in PNR.CAPACITIES},
                          routed_netlist_sha256=digest(routed), timing_report_sha256=digest(timing))
            fmax = timing_data.get("fmax", {})
            if len(fmax) != 2 or not report["clock_constraints_verified"]:
                raise RuntimeError("Missing two-clock timing/constraint evidence")
            report["target_met"] = all(value["achieved"] >= value["constraint"] for value in fmax.values())
            report["timing_qualified"] = report["routed"] and report["target_met"]
            report["error_stage"] = None
    report["inputs_changed_during_run"] = [name for name, sha in hashes.items() if digest(ROOT / name) != sha]
    write(report_path, report)
    if args.report:
        compact = {k: v for k, v in report.items() if k != "synthesis_cells"}
        write(args.report.resolve(), compact)
    if report["inputs_changed_during_run"]:
        raise RuntimeError("Measurement source changed during run")
    if not args.no_route and report.get("route_returncode"):
        return report["route_returncode"]
    print(json.dumps({k: report.get(k) for k in ("post_route_physical_resources", "fmax", "decoder_probe_minus_frozen_baseline_resources")}, indent=2), flush=True)
    print("Qualification report: " + str(report_path), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print("measure-upload-codec-gpu: " + str(error), file=sys.stderr)
        sys.exit(1)
