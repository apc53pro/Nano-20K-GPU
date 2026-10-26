#!/usr/bin/env python3
"""Place and route a source-matched logical GPU serial probe, never the board top."""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import math
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("nano_pnr_tools", ROOT / "scripts/rtl-check.py")
HDL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HDL)
DEVICE, FAMILY = "GW2AR-LV18QN88C8/I7", "GW2A-18C"
TARGETS = {"clk": 100.8, "host_clk": 25.2}
CLOCK_NETS = {"clk": "core_outputs[105]", "host_clk": "core.host_clk"}
CAPACITIES = {"LUT4": 20736, "DFF": 15552, "BSRAM": 46, "MULT18X18": 48}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    def invalid_constant(value):
        raise ValueError("Nonfinite JSON number: " + value)
    value = json.loads(path.read_text(encoding="utf-8"), parse_constant=invalid_constant)
    if not isinstance(value, dict):
        raise RuntimeError("Expected a JSON object: " + str(path))
    return value


def expected_sources(root):
    paths = {"rtl/uv_mult.v", "rtl/uv_pipeline.v", "rtl/gpu_probe.sv", "rtl/gpu_probe.cst",
             "scripts/rtl-core-pack.py", "scripts/rtl-generate-rom.py", "scripts/rtl-check.py"}
    paths.update(p.relative_to(root).as_posix() for p in (root / "rtl/gpu").glob("*.sv"))
    paths.update(p.relative_to(root).as_posix() for p in (root / "rtl/generated").glob("*.hex"))
    return paths


def within(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def source_path(root, name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise RuntimeError("Invalid source path in pack proof: " + name)
    result = root.joinpath(*path.parts)
    if not within(result.resolve(), root.resolve()):
        raise RuntimeError("Pack source escapes the repository: " + name)
    return result


def current_changes(root, pack_dir, proof):
    changed = sorted(expected_sources(root) - set(proof["source_sha256"]))
    for name, expected in proof["source_sha256"].items():
        path = source_path(root, name)
        if not path.is_file() or digest(path) != expected:
            changed.append(name)
    for name, field in (("seed_rom.v", "generated_rom_sha256"),
                        ("gpu-probe-synth.json", "netlist_sha256"),
                        ("gpu-probe-packed.json", "packed_netlist_sha256")):
        path = pack_dir / name
        if not path.is_file() or digest(path) != proof.get(field):
            changed.append(str(path))
    return sorted(set(changed))


def clock_aliases(design):
    module = design.get("modules", {}).get("nano_gpu_probe")
    if not module:
        raise RuntimeError("Synthesis artifact has no nano_gpu_probe top")
    result = {}
    for clock in TARGETS:
        port = module.get("ports", {}).get(clock, {})
        if port.get("direction") != "input" or len(port.get("bits", [])) != 1:
            raise RuntimeError("Missing scalar input clock: " + clock)
        bits = set(port["bits"])
        while True:
            previous = len(bits)
            for cell in module.get("cells", {}).values():
                conn = cell.get("connections", {})
                if cell["type"] in ("IBUF", "BUFG", "BUF") and set(conn.get("I", [])) & bits:
                    bits.update(conn.get("O", []))
            if len(bits) == previous:
                break
        names = {clock}
        for name, net in module.get("netnames", {}).items():
            values = net["bits"]
            for index, bit in enumerate(values):
                if bit in bits:
                    suffix = net.get("offset", 0) + (len(values)-1-index if net.get("upto", 0) else index)
                    names.add(name if len(values) == 1 else "{}[{}]".format(name, suffix))
        result[clock] = names
    if result["clk"] & result["host_clk"]:
        raise RuntimeError("The two probe clock domains alias one another")
    return result


def validate_pack(root, pack_dir):
    proof = read_json(pack_dir / "qualification.json")
    for name, expected in (("schema", "N20K-CORE-PACK-PROBE-1"), ("top", "nano_gpu_probe"),
                           ("device", DEVICE), ("family", FAMILY)):
        if proof.get(name) != expected:
            raise RuntimeError("Wrong or missing pack proof field: " + name)
    for name in ("elaboration_passed", "synthesized", "packed", "reported_resources_within_capacity"):
        if proof.get(name) is not True:
            raise RuntimeError("Pack proof is not successful: " + name)
    if (any(type(proof.get(k)) is not int or proof[k] != 0 for k in ("synthesis_returncode", "pack_returncode")) or
            proof.get("inputs_changed_during_run") != [] or proof.get("packer_errors") != []):
        raise RuntimeError("Pack proof contains a failed or changing-input run")
    hashes = proof.get("source_sha256")
    if not isinstance(hashes, dict) or not expected_sources(root).issubset(hashes):
        raise RuntimeError("Pack proof omits current implementation inputs")
    if any(not isinstance(h, str) or not re.fullmatch(r"[0-9a-f]{64}", h) for h in hashes.values()):
        raise RuntimeError("Malformed source digest in pack proof")
    for name, capacity in CAPACITIES.items():
        item = proof.get("packer_utilization", {}).get(name, {})
        if (type(item.get("used")) is not int or not 0 <= item["used"] <= capacity or
                item.get("available") != capacity or item.get("within_capacity") is not True):
            raise RuntimeError("Invalid or over-capacity packed resource: " + name)
    lower = proof.get("physical_lut_lower_bound", {})
    if (lower.get("within_capacity") is not True or lower.get("available_lut_sites") != CAPACITIES["LUT4"] or
            type(lower.get("minimum_physical_lut_sites")) is not int or
            not proof["packer_utilization"]["LUT4"]["used"] <= lower["minimum_physical_lut_sites"] <= CAPACITIES["LUT4"]):
        raise RuntimeError("Pack proof has no passing physical LUT lower bound")
    changes = current_changes(root, pack_dir, proof)
    if changes:
        raise RuntimeError("Stale pack inputs: " + ", ".join(changes))
    packed = read_json(pack_dir / "gpu-probe-packed.json").get("modules", {}).get("top", {})
    if not setting(packed.get("settings", {}).get("pack")):
        raise RuntimeError("Retained packed artifact has no completed pack setting")
    return proof, clock_aliases(read_json(pack_dir / "gpu-probe-synth.json"))


def setting(value):
    return isinstance(value, str) and bool(re.fullmatch(r"0*1", value))


def validate_timing(timing, aliases):
    raw = timing.get("fmax", {})
    if not isinstance(raw, dict) or not raw:
        raise RuntimeError("Timing report has no clock Fmax results")
    fmax, clocks, used = {}, {}, set()
    for name, entry in raw.items():
        if not isinstance(entry, dict):
            raise RuntimeError("Malformed clock result: " + name)
        values = [entry.get(k) for k in ("achieved", "constraint")]
        if any(type(v) not in (float, int) or not math.isfinite(v) or v <= 0 for v in values):
            raise RuntimeError("Invalid clock result: " + name)
        fmax[name] = {"achieved": values[0], "constraint": values[1]}
    for name, target in TARGETS.items():
        matches = set(fmax) & aliases[name]
        if len(matches) != 1:
            raise RuntimeError("Missing or ambiguous timing clock: " + name)
        matched = matches.pop()
        result = fmax[matched]
        requested_ps = 1e6 / target
        reported_ps = 1e6 / result["constraint"]
        # nextpnr stores integral-picosecond clock periods. Compare periods,
        # rather than a loose frequency percentage, and never relax the goal.
        if abs(reported_ps-requested_ps) > 1.0 + 1e-6:
            raise RuntimeError("Incorrect timing constraint for " + name)
        clocks[name] = dict(result, reported_clock=matched, requested_mhz=target,
                           requested_period_ps=requested_ps, reported_period_ps=reported_ps,
                           constraint_period_tolerance_ps=1.0,
                           target_met=result["achieved"] >= max(target, result["constraint"]))
        used.add(matched)
    if used != set(fmax):
        raise RuntimeError("Unexpected clocks in timing report: " + ", ".join(sorted(set(fmax)-used)))
    return {"fmax": fmax, "clocks": clocks, "target_met": all(c["target_met"] for c in clocks.values())}


def critical_paths(timing):
    paths = []
    entries = timing.get("critical_paths", [])
    if not isinstance(entries, list):
        raise RuntimeError("Malformed critical-path report")
    for entry in entries[:6]:
        if not isinstance(entry, dict) or not isinstance(entry.get("path", []), list):
            raise RuntimeError("Malformed critical-path entry")
        steps = entry.get("path", [])
        if any(not isinstance(s, dict) for s in steps):
            raise RuntimeError("Malformed critical-path step")
        delays = [s.get("delay", 0) for s in steps]
        if any(type(d) not in (float, int) or not math.isfinite(d) or d < 0 for d in delays):
            raise RuntimeError("Invalid critical-path delay")
        paths.append({"from_clock": entry.get("from"), "to_clock": entry.get("to"),
                      "from_cell": str(steps[0].get("from", {}).get("cell", ""))[:240] if steps else "",
                      "to_cell": str(steps[-1].get("to", {}).get("cell", ""))[:240] if steps else "",
                      "delay_ns": sum(delays)})
    return paths


def physical_resources(cells):
    """Count occupied physical BELs, reconstructing blockers removed after place.

    An ALU reserves its LUT site. RAM16SDP reserves eight LUTs and six FFs.
    nextpnr removes BLOCKER cells after placement, so raw cell-type totals
    alone undercount the final footprint. Union their BELs to avoid counting
    an occupied site twice when blockers are still present.
    """
    if not isinstance(cells, dict) or not cells:
        raise RuntimeError("Routed artifact has no physical cells")
    if any(not isinstance(cell, dict) or not isinstance(cell.get("type"), str) for cell in cells.values()):
        raise RuntimeError("Malformed routed physical cell")
    occupied = {name: set() for name in CAPACITIES}
    brams = {"SDP", "SP", "DP", "ROM", "SDPB", "SPX9", "DPX9", "SDPX9", "ROMX9", "pROM", "pROMX9"}
    for cell in cells.values():
        kind = cell["type"]
        lut = kind in ("LUT1", "LUT2", "LUT3", "LUT4", "BLOCKER_LUT", "ALU")
        ff = kind.startswith("DFF") or kind == "BLOCKER_FF"
        ram16 = kind in ("RAM16SDP4", "RAM16SDP2", "RAM16SDP1")
        if not (lut or ff or ram16 or kind in brams or kind == "MULT18X18"):
            continue
        bel = cell.get("attributes", {}).get("NEXTPNR_BEL", "")
        match = re.fullmatch(r"(X[0-9]+Y[0-9]+)/([^/]+)", bel)
        if not match:
            raise RuntimeError("Missing or malformed placed BEL for " + kind)
        tile, site = match.groups()
        if ram16:
            if site not in ("RAM16SDP4", "RAM16SDP2", "RAM16SDP1"):
                raise RuntimeError("Wrong distributed RAM BEL: " + bel)
            occupied["LUT4"].update((tile, i) for i in range(8))
            occupied["DFF"].update((tile, i) for i in range(6))
        elif lut or ff:
            expected = r"DFF([0-5])" if ff else r"ALU([0-5])" if kind == "ALU" else r"LUT([0-7])"
            index = re.fullmatch(expected, site)
            if not index:
                raise RuntimeError("Wrong logic BEL: " + bel)
            occupied["DFF" if ff else "LUT4"].add((tile, int(index[1])))
        else:
            occupied["BSRAM" if kind in brams else "MULT18X18"].add(bel)
    return {name: {"used": len(occupied[name]), "available": capacity, "within_capacity": len(occupied[name]) <= capacity}
            for name, capacity in CAPACITIES.items()}


def run_pnr(command, env, logfile, heartbeat_seconds=12):
    started = time.monotonic()
    stop = threading.Event()
    print("PnR log: " + str(logfile), flush=True)

    def heartbeat():
        while not stop.wait(heartbeat_seconds):
            print("PnR still running ({:.0f}s); log: {}".format(time.monotonic()-started, logfile), flush=True)

    monitor = threading.Thread(target=heartbeat, name="gpu-pnr-heartbeat", daemon=True)
    monitor.start()
    try:
        with logfile.open("w", encoding="utf-8") as log:
            return subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    finally:
        stop.set()
        monitor.join(timeout=1)


def write_report(path, report):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def pnr_command(prefix, build, database=None):
    command = prefix + ["--device", DEVICE, "-o", "family=" + FAMILY, "-o",
        "cst=" + (build / "source_snapshot/rtl/gpu_probe.cst").as_posix(),
        "--json", (build / "synth.json").as_posix(), "--write", (build / "routed.json").as_posix(),
        "--pre-place", (build / "clocks.py").as_posix(), "--freq", "100.8", "--threads", "2", "--seed", "1",
        "--timing-allow-fail", "--report", (build / "timing.json").as_posix(), "--detailed-timing-report"]
    return command + (["--chipdb", str(database)] if database else [])


def check_unchanged(pack, build, proof, report, analyzer_hash):
    changes = current_changes(ROOT, pack, proof)
    if digest(pack / "qualification.json") != report["pack_report_sha256"]:
        changes.append("pack qualification.json")
    if digest(Path(__file__)) != analyzer_hash:
        changes.append("scripts/rtl-core-pnr.py")
    changes += ["snapshot:" + name for name, expected in report["snapshot_sha256"].items()
                if not source_path(build, name).is_file() or digest(source_path(build, name)) != expected]
    report["inputs_changed_during_run"] = sorted(changes)
    if changes:
        report.update(current_inputs_verified=False, placed=False, routed=False,
                      timing_qualified=False, error_stage="inputs_changed")
        raise RuntimeError("Inputs changed during PnR/analysis: " + ", ".join(changes))


def finalize_run(report, build, proof, aliases, text):
    report["error_stage"] = "routing_evidence"
    routed = read_json(build / "routed.json").get("modules", {}).get("top", {})
    report["placed"] = setting(routed.get("settings", {}).get("place"))
    report["routed"] = report["placed"] and setting(routed.get("settings", {}).get("route"))
    if not report["routed"]:
        raise RuntimeError("nextpnr did not retain a placed and routed design")
    report["routed_netlist_sha256"] = digest(build / "routed.json")
    report["error_stage"] = "physical_capacity"
    resources = physical_resources(routed.get("cells"))
    report.update(post_route_physical_resources=resources,
                  post_route_resources_within_capacity=all(r["within_capacity"] for r in resources.values()),
                  post_route_counts_include_passthrough_luts=True,
                  post_route_resource_scope="Occupied BEL union, including ALU-reserved LUTs and RAM16 LUT/FF reservations",
                  post_route_explicit_cell_types=dict(sorted(Counter(c["type"] for c in routed["cells"].values()).items())),
                  post_route_lut_site_delta=resources["LUT4"]["used"]-proof["packer_utilization"]["LUT4"]["used"])
    if not report["post_route_resources_within_capacity"]:
        raise RuntimeError("Routed physical cell occupancy exceeds the selected FPGA capacity")
    report["error_stage"] = "timing_evidence"
    for clock, target in TARGETS.items():
        matches = re.findall(r"^Info:\s+constraining clock net '" + re.escape(CLOCK_NETS[clock]) + r"' to ([0-9.]+) MHz", text, re.MULTILINE)
        if not matches or not math.isclose(float(matches[-1]), target, abs_tol=0.001):
            raise RuntimeError("Clock constraint was not confirmed by nextpnr: " + clock)
    timing = read_json(build / "timing.json")
    checked = validate_timing(timing, aliases)
    report.update(checked, timing_qualified=checked["target_met"], critical_paths=critical_paths(timing),
                  utilization=timing.get("utilization", {}), timing_report_sha256=digest(build / "timing.json"),
                  log_sha256=digest(build / "pnr.log"), clock_constraints_confirmed=True, error_stage=None)


EVIDENCE_FILES = ("producer-runner.py", "producer-qualification.json", "routed.json", "timing.json",
                  "pnr.log", "synth.json", "seed_rom.v", "pack-proof.json", "clocks.py")


def capture_evidence(build, report):
    write_report(build / "producer-qualification.json", report)
    write_report(build / "retained-evidence.json", {
        "schema": "N20K-PNR-RETAINED-EVIDENCE-1", "hashes_captured_by": "producer",
        "sha256": {name: digest(build / name) for name in EVIDENCE_FILES if (build / name).is_file()}})


def retained_manifest(build):
    manifest = read_json(build / "retained-evidence.json")
    hashes = manifest.get("sha256", {})
    if (manifest.get("schema") != "N20K-PNR-RETAINED-EVIDENCE-1" or
            not isinstance(manifest.get("hashes_captured_by"), str) or not isinstance(hashes, dict) or
            not set(EVIDENCE_FILES).issubset(hashes)):
        raise RuntimeError("Incomplete retained evidence manifest")
    for name, expected in hashes.items():
        path = source_path(build, name)
        if not path.is_file() or digest(path) != expected:
            raise RuntimeError("Retained evidence digest mismatch: " + name)
    return manifest


def analyze_existing(pack, build, output, require_timing):
    report = {"schema": "N20K-CORE-PNR-PROBE-1", "whole_gpu_fit_qualified": False,
              "board_pinout_qualified": False, "production_pll_tmds_integrated": False,
              "placed": False, "routed": False, "timing_qualified": False,
              "current_inputs_verified": False, "error_stage": "retained_evidence"}
    write_report(output, report)
    started = time.monotonic()
    analyzer_hash = digest(Path(__file__))
    try:
        proof, aliases = validate_pack(ROOT, pack)
        manifest = retained_manifest(build)
        producer = read_json(build / "producer-qualification.json")
        if (producer.get("schema") != "N20K-CORE-PNR-PROBE-1" or producer.get("returncode") != 0 or
                producer.get("current_inputs_verified") is not True or producer.get("placed") is not True or
                producer.get("routed") is not True or producer.get("inputs_changed_during_run") != [] or
                producer.get("device") != DEVICE or producer.get("family") != FAMILY):
            raise RuntimeError("Producer did not retain a successful, current-input route")
        if digest(build / "producer-runner.py") != producer.get("runner_sha256"):
            raise RuntimeError("Retained producer script does not match its original hash")
        for name in ("source_sha256", "netlist_sha256", "generated_rom_sha256"):
            if producer.get(name) != proof.get(name):
                raise RuntimeError("Producer/pack provenance mismatch: " + name)
        if producer.get("routed_netlist_sha256") != manifest["sha256"]["routed.json"]:
            raise RuntimeError("Routed artifact differs from producer-attested hash")
        attested = {"routed.json": producer["routed_netlist_sha256"]}
        for name, expected in producer.get("raw_artifact_sha256", {}).items():
            if manifest["sha256"].get(name) != expected:
                raise RuntimeError("Artifact differs from producer-attested hash: " + name)
            attested[name] = expected
        required_snapshots = {"source_snapshot/" + name for name in proof["source_sha256"]}
        required_snapshots.update(("synth.json", "seed_rom.v", "pack-proof.json", "clocks.py"))
        if not required_snapshots.issubset(producer.get("snapshot_sha256", {})):
            raise RuntimeError("Producer snapshot provenance is incomplete")
        command = producer.get("command", [])
        if not isinstance(command, list) or command.count("--device") != 1 or not all(isinstance(c, str) for c in command):
            raise RuntimeError("Invalid retained native command")
        prefix = command[:command.index("--device")]
        database = command[command.index("--chipdb")+1] if "--chipdb" in command else None
        if not prefix or command != pnr_command(prefix, build, database):
            raise RuntimeError("Retained command does not match the exact probe/clock flow")
        report = {k: v for k, v in producer.items() if k not in ("error", "error_stage")}
        report.update(analysis_of_existing_run=True, analysis_runner_sha256=analyzer_hash,
                      producer_runner_sha256=producer["runner_sha256"],
                      producer_report_sha256=digest(build / "producer-qualification.json"),
                      retained_evidence_sha256=digest(build / "retained-evidence.json"),
                      retained_evidence_hashes_captured_by=manifest["hashes_captured_by"],
                      retained_artifact_sha256=manifest["sha256"], producer_command_validated=True,
                      producer_attested_artifact_sha256=attested,
                      recovery_captured_artifact_sha256={name: manifest["sha256"][name]
                          for name in ("timing.json", "pnr.log") if name not in attested},
                      retained_evidence_note=manifest.get("note"),
                      whole_gpu_fit_qualified=False, board_pinout_qualified=False, production_pll_tmds_integrated=False,
                      timing_qualified=False, error_stage="retained_evidence")
        check_unchanged(pack, build, proof, report, analyzer_hash)
        text = (build / "pnr.log").read_text(errors="replace")
        if "Program finished normally" not in text or re.search(r"^ERROR:", text, re.MULTILINE):
            raise RuntimeError("Retained log does not confirm normal native completion")
        finalize_run(report, build, proof, aliases, text)
        retained_manifest(build)
        check_unchanged(pack, build, proof, report, analyzer_hash)
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        report.update(error=str(error), timing_qualified=False)
    report["analysis_elapsed_seconds"] = round(time.monotonic()-started, 3)
    return publish(report, build, output, require_timing)


def publish(report, build, output, require_timing):
    write_report(build / "qualification.json", report)
    write_report(output, report)
    print("Serial probe PnR report: " + str(output), flush=True)
    if report.get("error"):
        print("rtl-core-pnr: " + report["error"], file=sys.stderr)
        return 1
    print("Probe routed; timing targets " + ("met." if report["timing_qualified"] else "NOT met."))
    return 2 if require_timing and not report["timing_qualified"] else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack-dir", type=Path, default=ROOT / "build/rtl-core-pack")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/rtl-core-pnr")
    parser.add_argument("--report", type=Path, default=ROOT / "docs/hardware/rtl-core-pnr.json")
    parser.add_argument("--require-timing", action="store_true")
    parser.add_argument("--analyze-existing", action="store_true", help="validate retained producer/artifact provenance and reanalyze without running tools")
    args = parser.parse_args(argv)
    pack, build, output = args.pack_dir.resolve(), args.build_dir.resolve(), args.report.resolve()
    if build == pack or within(output, pack):
        print("rtl-core-pnr: PnR outputs must not overwrite retained pack inputs", file=sys.stderr)
        return 1
    if args.analyze_existing:
        return analyze_existing(pack, build, output, args.require_timing)
    report = {"schema": "N20K-CORE-PNR-PROBE-1", "top": "nano_gpu_probe", "device": DEVICE, "family": FAMILY,
              "scope": "Complete logical GPU in the serial qualification probe; not a production board top",
              "whole_gpu_fit_qualified": False, "board_pinout_qualified": False, "production_pll_tmds_integrated": False,
              "placed": False, "routed": False, "timing_qualified": False, "current_inputs_verified": False,
              "clock_targets_mhz": TARGETS, "error_stage": "validation"}
    # Invalidate any old published success even when the new pack proof is stale.
    write_report(output, report)
    started = time.monotonic()
    try:
        proof, aliases = validate_pack(ROOT, pack)
        if any(CLOCK_NETS[name] not in aliases[name] for name in TARGETS):
            raise RuntimeError("Probe clock aliases changed; update the post-pack clock profile")
        proof_hash = digest(pack / "qualification.json")
        runner_hash = digest(Path(__file__))
        build.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(Path(__file__), build / "producer-runner.py")
        if digest(build / "producer-runner.py") != runner_hash:
            raise RuntimeError("Runner changed while preserving producer source")
        for name in ("routed.json", "timing.json", "pnr.log"):
            (build / name).unlink(missing_ok=True)
        for name, expected in proof["source_sha256"].items():
            target = build / "source_snapshot" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_path(ROOT, name), target)
            if digest(target) != expected:
                raise RuntimeError("Source changed while copying snapshot: " + name)
        for name, target, expected in (("gpu-probe-synth.json", "synth.json", proof["netlist_sha256"]),
                                      ("seed_rom.v", "seed_rom.v", proof["generated_rom_sha256"]),
                                      ("qualification.json", "pack-proof.json", proof_hash)):
            shutil.copyfile(pack / name, build / target)
            if digest(build / target) != expected:
                raise RuntimeError("Pack artifact changed while copying snapshot: " + name)
        cst = build / "source_snapshot/rtl/gpu_probe.cst"
        clock_script = build / "clocks.py"
        # Input-pad constraints do not survive this architecture's buffer
        # insertion. Constrain the verified core-side nets AFTER packing.
        clock_script.write_text("".join('ctx.addClock({}, {})\n'.format(json.dumps(CLOCK_NETS[name]), target)
                                        for name, target in TARGETS.items()), encoding="utf-8")
        snapshots = {p.relative_to(build).as_posix(): digest(p) for p in (build / "source_snapshot").rglob("*") if p.is_file()}
        snapshots.update({name: digest(build / name) for name in ("synth.json", "seed_rom.v", "pack-proof.json", "clocks.py")})
        report.update(source_sha256=proof["source_sha256"], netlist_sha256=proof["netlist_sha256"],
                      generated_rom_sha256=proof["generated_rom_sha256"], pack_report_sha256=proof_hash,
                      runner_sha256=runner_hash, snapshot_sha256=snapshots,
                      initial_packed_resources={name: proof["packer_utilization"][name] for name in CAPACITIES},
                      clock_constraint_nets=CLOCK_NETS,
                      current_inputs_verified=True, error_stage="toolchain")
        env = HDL.environment()
        if HDL.SUITE.is_dir():
            env["PYTHONHOME"] = str(HDL.SUITE)
        tool = HDL.tool("nextpnr-himbaechel")
        version = subprocess.run(tool + ["--version"], cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, check=True)
        report["nextpnr_version"] = version.stdout.strip()
        database = HDL.SUITE / "share/nextpnr/himbaechel/gowin/chipdb-GW2A-18C.bin"
        command = pnr_command(tool, build, database if database.is_file() else None)
        report.update(command=command, error_stage="place_and_route")
        print("Placing/routing the serial probe: core 100.8 MHz, host 25.2 MHz.", flush=True)
        result = run_pnr(command, env, build / "pnr.log")
        report["returncode"] = result.returncode
        text = (build / "pnr.log").read_text(errors="replace")
        report["tool_errors"] = re.findall(r"^ERROR:\s*(.*)$", text, re.MULTILINE)
        report["raw_artifact_sha256"] = {name: digest(build / name)
            for name in ("pnr.log", "timing.json", "routed.json") if (build / name).is_file()}
        check_unchanged(pack, build, proof, report, runner_hash)
        if result.returncode:
            report["placed"] = bool(re.search(r"^Info:\s+Routing(?: globals|\.\.)", text, re.MULTILINE))
            report["error_stage"] = ("tool_crash" if result.returncode < 0 else "placement" if "legal placement" in text
                                     else "routing" if report["placed"] else "tool")
            raise RuntimeError("nextpnr returned " + str(result.returncode))
        finalize_run(report, build, proof, aliases, text)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        report["error"] = str(error)
    report["elapsed_seconds"] = round(time.monotonic() - started, 3)
    if report.get("runner_sha256") and (build / "producer-runner.py").is_file():
        capture_evidence(build, report)
    return publish(report, build, output, args.require_timing)



if __name__ == "__main__":
    sys.exit(main())
