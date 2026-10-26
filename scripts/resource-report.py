#!/usr/bin/env python3
"""Audit model allocations, scoped RTL evidence and whole-GPU proof gaps.

This report is not a synthesis tool. The existing arithmetic qualification is
always a submodule result. The functional logical core has a separate build and
synthesis scope. Neither qualifies production board integration or full fit.
"""

import argparse
from collections import Counter
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PART = "GW2AR-LV18QN88C8/I7"
FAMILY = "GW2A-18C"
CAPACITIES = {"LUT4": 20736, "DFF": 15552, "BSRAM": 46, "MULT18X18": 48, "PLL": 2}
UV_REQUIRED_INPUTS = {
    "rtl/uv_mult.v", "rtl/uv_pipeline.v", "rtl/uv_probe.v", "rtl/uv_probe.cst",
    "rtl/tests/uv_pipeline_tb.v", "rtl/tests/generate_uv_vectors.cpp",
    "reference/tests/uv_oracle.hpp", "reference/src/hw_reciprocal.cpp",
    "reference/include/nano_gpu/hw/reciprocal.hpp",
    "reference/include/nano_gpu/hw/reciprocal_seed.hpp",
    "scripts/generate-reciprocal-rom.py", "scripts/rtl-generate-rom.py", "scripts/rtl-check.py",
    *("rtl/generated/reciprocal_seed_{}.hex".format(index) for index in range(4)),
}
CORE_TOP = "nano_gpu_core"
CORE_REQUIRED_INPUTS = {
    "rtl/uv_mult.v", "rtl/uv_pipeline.v",
    *("rtl/gpu/" + name + ".sv" for name in (
        "nano_fragment_pipeline", "nano_gpu_core", "nano_host_link",
        "nano_scanout", "nano_sdp16", "nano_sdram_service",
        "nano_texture_cache", "nano_tile_renderer", "nano_tmds")),
    *("reference/src/" + name + ".cpp" for name in (
        "setup", "renderer", "demo", "hw_protocol")),
    "reference/rtl/main.cpp", "reference/rtl/sdram_model.cpp",
    "reference/rtl/sdram_model.hpp",
    "scripts/gpu-rtl.py", "scripts/rtl-generate-rom.py", "scripts/rtl-check.py",
    *("rtl/generated/reciprocal_seed_{}.hex".format(index) for index in range(4)),
}
CORE_PACK_SCOPE = "Complete functional logical GPU in a serial qualification probe, including probe overhead"
CORE_PACK_OBSERVABILITY = {
    "probe_input_shift_bits": 50, "probe_output_snapshot_bits": 116,
    "probe_checksum_bits": 32, "probe_maximum_register_overhead": 198,
}
CORE_PACK_CAPACITIES = {
    "LUT4": 20736, "DFF": 15552, "ALU": 15552,
    "MULT18X18": 48, "BSRAM": 46, "RAM16SDP4": 648,
}
CORE_PACK_REQUIRED_INPUTS = {name for name in CORE_REQUIRED_INPUTS if name.startswith("rtl/")} | {
    "rtl/gpu_probe.sv", "rtl/gpu_probe.cst", "scripts/rtl-core-pack.py",
    "scripts/rtl-generate-rom.py", "scripts/rtl-check.py",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_path(repo, name):
    path = Path(name)
    if path.is_absolute() or not path.parts or ".." in path.parts:
        raise ValueError("source path must be repository-relative: " + str(name))
    result = (repo / path).resolve()
    if repo.resolve() not in result.parents:
        raise ValueError("source path escapes the repository: " + str(name))
    return result


def integer(value, name):
    if type(value) is not int or value < 0:
        raise ValueError(name + " must be a nonnegative integer")
    return value


def core_required_inputs(repo):
    # The runner compiles every GPU module and fingerprints the C++ adapter's
    # public headers. A newly added source cannot silently disappear from proof.
    paths = list((repo / "rtl/gpu").glob("*.sv"))
    paths += list((repo / "reference/include").rglob("*.hpp"))
    paths += list((repo / "reference/rtl").glob("*.hpp"))
    return CORE_REQUIRED_INPUTS | {str(path.relative_to(repo)) for path in paths}


def valid_digest(value):
    return (isinstance(value, str) and len(value) == 64 and
            all(character in "0123456789abcdef" for character in value))


def logical_core_summary(repo, report, report_path=None):
    """Check recorded evidence without rerunning tools or qualifying board fit."""
    repo = Path(repo).resolve()
    result = {
        "scope": "nano_gpu_core functional command-to-pixel logical core",
        "status": "unavailable", "top": CORE_TOP, "whole_gpu_evidence": False,
        "simulation_passed": None, "timing_met": None,
        "source_mismatches": [], "synthesis_status": "unavailable",
        "synthesis_cells": None, "recorded_synthesis_cells": None,
        "netlist_hash_verified": False, "netlist_cells_verified": False,
        "implementation_regenerated": False,
        "excluded_resources": [
            "Standalone TMDS encoder, serializers and differential output buffers",
            "Production clock/PLL and reset generation, board wrapper and package pins",
        ],
        "evidence_kind": "Recorded Verilator build; no simulation run is established. Optional mapped cell counts are checked against the recorded netlist, not regenerated.",
    }
    if report is None:
        return result
    if not isinstance(report, dict):
        result.update(status="invalid", reason="Logical-core report must be an object")
        return result
    result["recorded_synthesis_cells"] = copy.deepcopy(report.get("synthesis_cells"))
    result["recorded_schema"] = report.get("schema")
    if (report.get("schema") != "N20K-GPU-RTL-BUILD-1" or
            report.get("device") != PART or report.get("family") != FAMILY or
            report.get("top") != CORE_TOP or report.get("simulation_backend") != "verilator" or
            report.get("cpp_device_backends_linked") is not False or
            report.get("whole_gpu_fit_qualified") is not False or
            not isinstance(report.get("verilator_version"), str) or
            not report["verilator_version"].strip()):
        result.update(status="invalid", reason="Report does not identify the selected independent logical RTL build")
        return result
    hashes = report.get("source_sha256")
    if (not isinstance(hashes, dict) or
            not all(isinstance(name, str) and name for name in hashes) or
            not core_required_inputs(repo).issubset(hashes) or
            sum(Path(name).name == "seed_rom.v" for name in hashes) != 1 or
            not all(valid_digest(value) for value in hashes.values())):
        result.update(status="invalid", reason="Missing or invalid logical-core source/generated-ROM provenance")
        return result
    for name, expected in hashes.items():
        try:
            path = source_path(repo, name)
            if not path.is_file() or digest(path) != expected:
                result["source_mismatches"].append(name)
        except (TypeError, ValueError):
            result["source_mismatches"].append(name)
    if result["source_mismatches"]:
        result.update(status="stale", reason="Recorded logical-core inputs differ from the current source tree")
        return result
    result.update(status="source_matched_build", verilator_version=report["verilator_version"])
    if not any(key in report for key in ("synthesis_cells", "netlist_sha256", "yosys_version")):
        return result
    result["synthesis_status"] = "invalid"
    counts = report.get("synthesis_cells")
    if (not isinstance(counts, dict) or not counts or
            not all(isinstance(name, str) and name and type(count) is int and count >= 0
                    for name, count in counts.items()) or
            not valid_digest(report.get("netlist_sha256")) or
            not isinstance(report.get("yosys_version"), str) or
            not report["yosys_version"].strip() or report_path is None):
        result["synthesis_reason"] = "Missing or invalid mapped-netlist provenance/counts"
        return result
    netlist_path = Path(report_path).resolve().parent / "gpu-synth.json"
    try:
        if digest(netlist_path) != report["netlist_sha256"]:
            result["synthesis_reason"] = "Mapped netlist hash differs from the recorded synthesis"
            return result
        result["netlist_hash_verified"] = True
        design = json.loads(netlist_path.read_text(encoding="utf-8"))
        cells = design["modules"][CORE_TOP]["cells"]
        if not isinstance(cells, dict) or not cells:
            raise ValueError("logical-core netlist contains no mapped cells")
        actual = Counter()
        for cell in cells.values():
            cell_type = cell["type"]
            if not isinstance(cell_type, str) or not cell_type:
                raise ValueError("mapped cell type must be a nonempty string")
            actual[cell_type] += 1
        if dict(actual) != counts:
            result["synthesis_reason"] = "Recorded cell counts differ from the mapped netlist"
            return result
    except (OSError, ValueError, KeyError, TypeError) as error:
        result["synthesis_reason"] = "Cannot validate mapped netlist: " + str(error)
        return result
    result.update(synthesis_status="source_matched_recorded_synthesis",
                  synthesis_cells=dict(sorted(actual.items())), netlist_cells_verified=True,
                  yosys_version=report["yosys_version"])
    return result


def validate_pack_artifacts(report, build_report_path):
    """Optional check of retained generated files, without rerunning the packer."""
    path = Path(build_report_path).resolve()
    build = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(build, dict):
        raise ValueError("pack build report must be an object")
    keys = {"schema", "top", "device", "family", "scope", "source_sha256",
            "generated_rom_sha256", "netlist_sha256", "packed_netlist_sha256",
            "synthesized", "packed", "elaboration_passed", "inputs_changed_during_run",
            "whole_gpu_fit_qualified", "board_pinout_qualified", "production_pll_tmds_integrated",
            "routed", "timing_qualified", "synthesis_returncode", "pack_returncode"}
    keys |= CORE_PACK_OBSERVABILITY.keys()
    if any(build.get(key) != report.get(key) for key in keys):
        raise ValueError("retained build report differs from the recorded pack evidence")
    for resource in CORE_PACK_CAPACITIES:
        recorded = report["packer_utilization"][resource]
        actual = build["packer_utilization"][resource]
        if any(actual.get(key) != recorded[key] for key in ("used", "available")):
            raise ValueError("retained pack utilization differs for " + resource)
    if digest(path.parent / "seed_rom.v") != report["generated_rom_sha256"]:
        raise ValueError("generated seed ROM hash differs")
    for filename, hash_key, count_key, top in (
            ("gpu-probe-synth.json", "netlist_sha256", "synthesis_cells", "nano_gpu_probe"),
            ("gpu-probe-packed.json", "packed_netlist_sha256", "packed_cells", "top")):
        artifact = path.parent / filename
        if digest(artifact) != report[hash_key]:
            raise ValueError(filename + " hash differs")
        cells = json.loads(artifact.read_text(encoding="utf-8"))["modules"][top]["cells"]
        recorded = build.get(count_key)
        if (not isinstance(cells, dict) or not cells or not isinstance(recorded, dict) or
                not all(isinstance(name, str) and name and type(count) is int and count >= 0
                        for name, count in recorded.items())):
            raise ValueError("missing or invalid mapped cell inventory")
        counts = Counter()
        for cell in cells.values():
            kind = cell["type"]
            if not isinstance(kind, str) or not kind:
                raise ValueError("mapped cell type must be a nonempty string")
            counts[kind] += 1
        if dict(counts) != recorded:
            raise ValueError(count_key + " differs from the retained netlist")
        if top == "top":
            occupancy = {
                "LUT4": sum(counts[name] for name in ("LUT1", "LUT2", "LUT3", "LUT4", "BLOCKER_LUT")),
                "DFF": sum(value for name, value in counts.items() if name.startswith("DFF")) + counts["BLOCKER_FF"],
                "ALU": counts["ALU"], "MULT18X18": counts["MULT18X18"],
                "BSRAM": sum(counts[name] for name in ("SP", "SDP", "DP", "ROM")),
                "RAM16SDP4": counts["RAM16SDP4"],
            }
            if any(occupancy[name] != report["packer_utilization"][name]["used"]
                   for name in CORE_PACK_CAPACITIES):
                raise ValueError("packer occupancy differs from retained packed cell sites")


def core_pack_summary(repo, report, build_report_path=None):
    """A source-matched serial probe can reject capacity, never qualify the board."""
    repo = Path(repo).resolve()
    result = {
        "scope": "Exact-part serial logical-core probe, including probe overhead",
        "status": "unavailable", "source_matched": False, "source_mismatches": [],
        "whole_gpu_evidence": False, "timing_met": None,
        "utilization": None, "recorded_utilization": None,
        "resource_capacity_met": None, "capacity_rejected_resources": [],
        "capacity_overages": {}, "generated_artifacts_revalidated": False,
        "implementation_regenerated": False,
        "tool_provenance_complete": False,
        "evidence_kind": "Historical exact-part pack report checked against current source hashes; generated ROM/netlist artifacts are not revalidated unless a retained build report is supplied",
        "accounting_notes": [
            "LUT4 occupancy already includes ALU and distributed-RAM occupied LUT sites; do not add ALU to LUT4",
            "Counts include the serial observability probe; no probe-overhead subtraction is inferred",
            "IOB availability in packer reports is a family limit, not a qualified QN88 board pin budget",
            "Production PLL/TMDS integration, placement, routing and timing remain unqualified",
            "Tool and chip-database provenance is not complete physical qualification",
        ],
    }
    if report is None:
        return result
    if not isinstance(report, dict):
        result.update(status="invalid", reason="Pack report must be an object")
        return result
    result["recorded_utilization"] = copy.deepcopy(report.get("packer_utilization"))
    if (report.get("schema") != "N20K-CORE-PACK-PROBE-1" or
            report.get("device") != PART or report.get("family") != FAMILY or
            report.get("top") != "nano_gpu_probe" or report.get("scope") != CORE_PACK_SCOPE or
            any(report.get(key) != value or type(report.get(key)) is not int
                for key, value in CORE_PACK_OBSERVABILITY.items()) or
            any(report.get(key) is not False for key in (
                "whole_gpu_fit_qualified", "board_pinout_qualified", "production_pll_tmds_integrated",
                "routed", "timing_qualified"))):
        result.update(status="invalid", reason="Report does not identify the selected serial probe and observability scope")
        return result
    hashes = report.get("source_sha256")
    required = CORE_PACK_REQUIRED_INPUTS | {
        str(path.relative_to(repo)) for path in (repo / "rtl/gpu").glob("*.sv")}
    if (not isinstance(hashes, dict) or not required.issubset(hashes) or
            not all(isinstance(name, str) and name and valid_digest(value)
                    for name, value in hashes.items()) or
            not all(valid_digest(report.get(key)) for key in (
                "generated_rom_sha256", "netlist_sha256", "packed_netlist_sha256"))):
        result.update(status="invalid", reason="Missing or invalid core/probe/constraint/generated provenance")
        return result
    for name, expected in hashes.items():
        try:
            path = source_path(repo, name)
            if not path.is_file() or digest(path) != expected:
                result["source_mismatches"].append(name)
        except (TypeError, ValueError):
            result["source_mismatches"].append(name)
    if result["source_mismatches"]:
        result.update(status="stale", reason="Recorded pack inputs differ from the current source tree")
        return result
    if (any(report.get(key) is not True for key in ("elaboration_passed", "synthesized", "packed")) or
            any(type(report.get(key)) is not int or report[key] != 0
                for key in ("synthesis_returncode", "pack_returncode")) or
            report.get("inputs_changed_during_run") != [] or report.get("packer_errors") != []):
        result.update(status="invalid", reason="Stable-input elaboration, synthesis and packing did not complete cleanly")
        return result
    reported = report.get("packer_utilization")
    if not isinstance(reported, dict) or not CORE_PACK_CAPACITIES.keys() <= reported.keys():
        result.update(status="invalid", reason="Required exact-part occupancy counts are missing")
        return result
    utilization = {}
    for resource, available in CORE_PACK_CAPACITIES.items():
        row = reported[resource]
        if (not isinstance(row, dict) or type(row.get("used")) is not int or row["used"] < 0 or
                type(row.get("available")) is not int or row["available"] != available):
            result.update(status="invalid", reason="Invalid occupancy or selected-part capacity for " + resource)
            return result
        used = row["used"]
        utilization[resource] = {"used": used, "available": available,
                                 "within_capacity": used <= available,
                                 "overage": max(0, used - available)}
    if build_report_path is not None:
        try:
            validate_pack_artifacts(report, build_report_path)
        except (OSError, ValueError, KeyError, TypeError) as error:
            result.update(status="invalid", reason="Cannot revalidate retained pack artifacts: " + str(error))
            return result
        result["generated_artifacts_revalidated"] = True
    overages = {name: row["overage"] for name, row in utilization.items() if row["overage"]}
    result.update(status="capacity_rejected" if overages else "source_matched_capacity_check",
                  source_matched=True, utilization=utilization,
                  resource_capacity_met=not overages, capacity_overages=overages,
                  capacity_rejected_resources=sorted(overages),
                  probe_register_overhead_maximum=CORE_PACK_OBSERVABILITY["probe_maximum_register_overhead"])
    return result


def probe_summary(repo, report):
    result = {
        "scope": "recorded reciprocal/UV arithmetic probe, including serial test harness",
        "status": "unavailable", "utilization": None, "recorded_utilization": None,
        "source_mismatches": [], "whole_gpu_evidence": False,
        "generated_artifacts_revalidated": False, "timing_met": None,
        "evidence_kind": "Historical arithmetic report with source-hash checks; generated netlist/report artifacts are not regenerated by this audit",
    }
    if report is None:
        return result
    result["recorded_utilization"] = copy.deepcopy(report.get("utilization"))
    result["recorded_target_met"] = report.get("target_met")
    result["recorded_fmax"] = copy.deepcopy(report.get("fmax"))
    result["recorded_schema"] = report.get("schema")
    result["harness_register_bits"] = report.get("probe_stimulus_bits", 0) + report.get("probe_checksum_bits", 0)
    if (report.get("schema") != "N20K-UV-RTL-0.1" or report.get("device") != PART or
            report.get("family") != FAMILY):
        result.update(status="invalid", reason="Report schema or device does not identify the selected arithmetic probe")
        return result
    hashes = report.get("source_sha256")
    if not isinstance(hashes, dict) or not UV_REQUIRED_INPUTS.issubset(hashes):
        result.update(status="invalid", reason="Missing arithmetic source/constraint provenance")
        return result
    generated = report.get("generated_sha256")
    if not isinstance(generated, dict) or not {"seed_rom.v", "uv-vectors.txt"}.issubset(generated):
        result.update(status="invalid", reason="Missing generated ROM/vector provenance")
        return result
    for name, expected in hashes.items():
        try:
            path = source_path(repo, name)
            if not path.is_file() or digest(path) != expected:
                result["source_mismatches"].append(name)
        except ValueError:
            result["source_mismatches"].append(name)
    if result["source_mismatches"]:
        result.update(status="stale", reason="Recorded inputs differ from the current source tree")
    elif not all(report.get(key) is True for key in ("simulation_passed", "synthesized", "routed")):
        result.update(status="invalid", reason="Arithmetic simulation, synthesis or routing did not complete")
    elif not isinstance(report.get("utilization"), dict):
        result.update(status="invalid", reason="No recorded routed utilization")
    else:
        result.update(status="qualified_submodule", utilization=copy.deepcopy(report["utilization"]))
        clocks = report.get("fmax", {})
        if isinstance(clocks, dict) and clocks:
            try:
                result["timing_met"] = all(float(clock["achieved"]) > 0 and
                    float(clock["constraint"]) > 0 and float(clock["achieved"]) >= float(clock["constraint"])
                    for clock in clocks.values())
            except (KeyError, TypeError, ValueError):
                result["timing_met"] = None
    # A source-matched historical submodule report does not regenerate its
    # netlist, certify a production top, or qualify the package pin budget.
    return result


def build_report(repo, model_profile, manifest, probe_report=None,
                 core_report=None, core_report_path=None,
                 core_pack_report=None, core_pack_build_report_path=None):
    repo = Path(repo).resolve()
    if model_profile.get("part") != PART or manifest.get("device") != PART or manifest.get("family") != FAMILY:
        raise ValueError("resource evidence must target " + PART + " / " + FAMILY)
    if manifest.get("schema") != "N20K-FULLGPU-MANIFEST-1":
        raise ValueError("unsupported full-GPU implementation manifest")
    capacity = {
        "LUT4": integer(model_profile.get("lut4"), "LUT4 capacity"),
        "DFF": integer(model_profile.get("flip_flops"), "DFF capacity"),
        "BSRAM": integer(model_profile.get("bsram_blocks"), "BSRAM capacity"),
        "MULT18X18": integer(model_profile.get("mult18_equivalents"), "MULT18X18 capacity"),
        "PLL": integer(model_profile.get("available_plls"), "PLL capacity"),
    }
    if capacity != CAPACITIES:
        raise ValueError("model capacities disagree with the selected exact device")
    system = model_profile.get("system_model")
    if not isinstance(system, dict):
        raise ValueError("a system-mode hardware profile is required")
    allocations = {
        "BSRAM": integer(system.get("native_bsram_blocks"), "selected BSRAM"),
        "MULT18X18": integer(system.get("render_mult18_equivalents"), "selected multipliers"),
        "LUT4": None, "DFF": None, "PLL": None, "IOB": None,
    }
    blocks = manifest.get("blocks")
    if not isinstance(blocks, list) or not blocks:
        raise ValueError("implementation manifest must list the required GPU blocks")
    inventory, missing, source_hashes, identifiers = [], [], {}, set()
    for block in blocks:
        name, status = block.get("id"), block.get("status")
        if not isinstance(name, str) or not name or name in identifiers:
            raise ValueError("required blocks need unique nonempty identifiers")
        identifiers.add(name)
        if status not in ("rtl_submodule", "modeled_only", "not_implemented"):
            raise ValueError("unsupported implementation status for " + name)
        row = copy.deepcopy(block)
        row["missing_sources"] = []
        rtl_sources = block.get("rtl_sources", [])
        for field in ("model_sources", "rtl_sources"):
            paths = block.get(field, [])
            if not isinstance(paths, list):
                raise ValueError(field + " must be a source-path list")
            for filename in paths:
                path = source_path(repo, filename)
                if path.is_file():
                    source_hashes[filename] = digest(path)
                else:
                    row["missing_sources"].append(filename)
        row["rtl_source_present"] = status == "rtl_submodule" and bool(rtl_sources) and not row["missing_sources"]
        if not row["rtl_source_present"]:
            missing.append(name)
        inventory.append(row)
    blockers = ["Missing RTL: " + name for name in missing]
    if not manifest.get("production_top"):
        blockers.append("Production GPU top is not implemented")
    if not manifest.get("constraints"):
        blockers.append("Production pin and external timing constraints are unresolved")
    if not manifest.get("required_clocks"):
        blockers.append("Required physical clocks, resets and crossings are unresolved")
    blockers.extend([
        "No production-board GPU qualification including physical clocking, TMDS integration and pins exists",
        "No production-board integrated synthesis and exact-package placement/routing qualification exists",
        "Full mapped LUT/FF/DSP/BSRAM/clock/I/O totals and all-clock timing are unavailable",
    ])
    for resource in ("BSRAM", "MULT18X18"):
        if allocations[resource] > capacity[resource]:
            blockers.append("Selected model allocation exceeds " + resource + " capacity")
    packed_core = core_pack_summary(repo, core_pack_report, core_pack_build_report_path)
    for resource, overage in packed_core["capacity_overages"].items():
        row = packed_core["utilization"][resource]
        blockers.append("Source-matched logical-core serial probe exceeds {} capacity: {} / {} (+{}; includes probe overhead)".format(
            resource, row["used"], row["available"], overage))
    return {
        "schema": "N20K-RESOURCE-REPORT-1",
        "device": {"part": PART, "family": FAMILY, "capacity": capacity},
        "allocation_scope": "Selected model native allocations; unknown values are null, not zero or remaining space",
        "profile_provenance_scope": "Hardware-profile snapshot; current source inventory hashes alone do not prove the executable was built from those sources. The Make target rebuilds its normal dependencies.",
        "known_allocations": allocations,
        "native_budget_within_capacity": all(allocations[key] <= capacity[key] for key in ("BSRAM", "MULT18X18")),
        "register_inventory": {
            **copy.deepcopy(model_profile.get("resource_accounting", {})),
            "scope": "Selected payload register ledger only; optional profiling is not established physical DFF use",
            "complete": False,
        },
        "implementation_blocks": inventory,
        "missing_blocks": missing,
        "source_sha256": dict(sorted(source_hashes.items())),
        "arithmetic_probe": probe_summary(repo, probe_report),
        "logical_core": logical_core_summary(repo, core_report, core_report_path),
        "logical_core_pack": packed_core,
        "whole_gpu": {
            "status": "incomplete", "fits": None, "timing_met": None, "rtl_complete": False,
            "mapped_resource_counts": None,
            "blockers": blockers,
            "required_proof": [
                "Complete functional RTL, including physical output, clocks and I/O resources; no behavioral stubs",
                "Functional and cycle comparisons against the SRR under stalls, reset and recovery",
                "Single integrated synthesis netlist with actual mapped cell/resource totals",
                "Successful placement and routing on the exact package with real pin and clock constraints",
                "All required clock and external timing checks passing, with unconstrained paths accounted for",
                "Matching source, generator, netlist, constraints, device database and tool provenance",
            ],
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=ROOT / "build/nano-gpu-ref-headless")
    parser.add_argument("--model-profile", type=Path, help="read a saved system hardware-profile JSON instead")
    parser.add_argument("--manifest", type=Path, default=ROOT / "rtl/full-gpu-manifest.json")
    parser.add_argument("--probe-report", type=Path, default=ROOT / "docs/hardware/rtl-qualification.json")
    parser.add_argument("--core-report", type=Path, default=ROOT / "build/rtl-gpu/build-report.json",
                        help="optional scoped logical-core build/synthesis evidence")
    parser.add_argument("--core-pack-report", type=Path, default=ROOT / "docs/hardware/rtl-core-pack.json",
                        help="optional historical exact-part serial-core packing evidence")
    parser.add_argument("--core-pack-build-report", type=Path,
                        help="also validate retained generated pack artifacts beside this build report")
    parser.add_argument("--output", type=Path, default=ROOT / "build/resource-report.json")
    parser.add_argument("--require-full-fit", action="store_true", help="exit 2 unless whole-GPU fit is proven")
    args = parser.parse_args()
    if args.model_profile:
        profile = json.loads(args.model_profile.read_text(encoding="utf-8"))
    else:
        result = subprocess.run([str(args.executable.resolve()), "--mode", "system", "--hardware-profile"],
                                check=True, capture_output=True, text=True)
        profile = json.loads(result.stdout)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    probe = json.loads(args.probe_report.read_text(encoding="utf-8")) if args.probe_report.is_file() else None
    core = json.loads(args.core_report.read_text(encoding="utf-8")) if args.core_report.is_file() else None
    packed = json.loads(args.core_pack_report.read_text(encoding="utf-8")) if args.core_pack_report.is_file() else None
    report = build_report(ROOT, profile, manifest, probe, core, args.core_report,
                          packed, args.core_pack_build_report)
    evidence = {"manifest": {"path": str(args.manifest), "sha256": digest(args.manifest)}}
    if args.model_profile:
        evidence["model_profile"] = {"path": str(args.model_profile), "sha256": digest(args.model_profile)}
    else:
        evidence["model_profile_executable"] = {"path": str(args.executable), "sha256": digest(args.executable)}
    if args.probe_report.is_file():
        evidence["recorded_probe_report"] = {"path": str(args.probe_report), "sha256": digest(args.probe_report)}
    if args.core_report.is_file():
        evidence["recorded_logical_core_report"] = {"path": str(args.core_report), "sha256": digest(args.core_report)}
    if args.core_pack_report.is_file():
        evidence["recorded_logical_core_pack"] = {"path": str(args.core_pack_report), "sha256": digest(args.core_pack_report)}
    if args.core_pack_build_report:
        evidence["retained_pack_build_report"] = {"path": str(args.core_pack_build_report), "sha256": digest(args.core_pack_build_report)}
    report["evidence"] = evidence
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if report["logical_core_pack"]["status"] == "capacity_rejected":
        print("Whole-GPU qualification: INCOMPLETE (logical-core probe exceeds capacity; production timing unknown)")
    else:
        print("Whole-GPU resource/fit status: INCOMPLETE (fit and timing unknown)")
    print("Selected model: {} / {} BSRAM; {} / {} native 18x18 equivalents".format(
        report["known_allocations"]["BSRAM"], CAPACITIES["BSRAM"],
        report["known_allocations"]["MULT18X18"], CAPACITIES["MULT18X18"]))
    print("Full-GPU LUT4, DFF, PLL and package I/O usage: unknown")
    print("Missing required RTL blocks: " + str(len(report["missing_blocks"])))
    for reason in report["whole_gpu"]["blockers"]:
        print("  - " + reason)
    print("Arithmetic probe: " + report["arithmetic_probe"]["status"] + " (never a full-GPU fit result)")
    print("Recorded arithmetic target timing met: " + str(report["arithmetic_probe"]["timing_met"]))
    print("Logical core: " + report["logical_core"]["status"] + " (production board excluded)")
    print("Logical-core synthesis: " + report["logical_core"]["synthesis_status"])
    if report["logical_core"]["synthesis_cells"] is not None:
        print("Recorded logical-core mapped cells: " + json.dumps(report["logical_core"]["synthesis_cells"], sort_keys=True))
    print("Exact-part serial-core pack: " + report["logical_core_pack"]["status"])
    for resource, overage in report["logical_core_pack"]["capacity_overages"].items():
        row = report["logical_core_pack"]["utilization"][resource]
        print("  {}: {} / {} (+{}; includes probe overhead)".format(resource, row["used"], row["available"], overage))
    print("Report: " + str(args.output))
    if args.require_full_fit:
        return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print("resource-report: " + str(error), file=sys.stderr)
        sys.exit(1)
