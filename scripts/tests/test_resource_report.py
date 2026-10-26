"""Resource budgets and arithmetic probes must never certify a complete GPU."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "scripts/resource-report.py"
SPEC = importlib.util.spec_from_file_location("resource_report", MODULE)
resources = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(resources)


def profile():
    return {
        "part": "GW2AR-LV18QN88C8/I7",
        "architecture": "GW2AR-18C",
        "lut4": 20736,
        "flip_flops": 15552,
        "bsram_blocks": 46,
        "mult18_equivalents": 48,
        "available_plls": 2,
        "system_model": {
            "native_bsram_blocks": 39,
            "render_mult18_equivalents": 46,
            "storage_profile": "N20K-STORAGE-0.3",
        },
        "resource_accounting": {
            "selected_fixed_payload_register_bits": 6404,
            "optional_profiling_counter_bits": 512,
            "state_inventory_complete": False,
        },
    }


def manifest():
    return {
        "schema": "N20K-FULLGPU-MANIFEST-1",
        "device": "GW2AR-LV18QN88C8/I7",
        "family": "GW2A-18C",
        "production_top": None,
        "constraints": None,
        "required_clocks": {"core": None, "sdram": None, "host": None, "pixel": None},
        "blocks": [
            {"id": "reciprocal_uv", "status": "rtl_submodule",
             "model_sources": ["reference/src/hw_reciprocal.cpp"],
             "rtl_sources": ["rtl/uv_pipeline.v", "rtl/uv_mult.v"]},
            {"id": "host_link", "status": "modeled_only",
             "model_sources": ["reference/src/hw_host_link.cpp"], "rtl_sources": []},
            {"id": "rasterizer", "status": "modeled_only",
             "model_sources": ["reference/src/hw_tile_pipeline.cpp"], "rtl_sources": []},
            {"id": "scanout", "status": "modeled_only",
             "model_sources": ["reference/src/hw_scanout.cpp"], "rtl_sources": []},
            {"id": "board_io_clocks", "status": "not_implemented",
             "model_sources": [], "rtl_sources": []},
        ],
    }


def fresh_probe():
    result = json.loads((ROOT / "docs/hardware/rtl-qualification.json").read_text())
    # Fixture freshness follows the current files, independently of whether
    # the checked-in historical report is stale after a later source change.
    result["source_sha256"] = {
        name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
        for name in result["source_sha256"]
    }
    return result


class ResourceReportTests(unittest.TestCase):
    def build(self, model=None, coverage=None, probe=None, core=None, core_path=None):
        return resources.build_report(ROOT, model or profile(), coverage or manifest(),
                                      probe, core, core_path)

    def assert_incomplete(self, report):
        self.assertEqual(report["whole_gpu"]["status"], "incomplete")
        self.assertIsNone(report["whole_gpu"]["fits"])
        self.assertIsNone(report["whole_gpu"]["timing_met"])
        self.assertFalse(report["whole_gpu"]["rtl_complete"])

    def test_current_budget_is_not_a_fit_proof(self):
        result = self.build()
        self.assert_incomplete(result)
        self.assertEqual(result["known_allocations"]["BSRAM"], 39)
        self.assertEqual(result["known_allocations"]["MULT18X18"], 46)
        self.assertTrue(result["missing_blocks"])
        for name in ("LUT4", "DFF", "PLL", "IOB"):
            with self.subTest(resource=name):
                self.assertIsNone(result["known_allocations"][name])

    def test_capacity_comes_from_exact_selected_part(self):
        result = self.build()
        self.assertEqual(result["device"]["part"], "GW2AR-LV18QN88C8/I7")
        self.assertEqual(result["device"]["family"], "GW2A-18C")
        self.assertEqual(result["device"]["capacity"], {
            "LUT4": 20736, "DFF": 15552, "BSRAM": 46, "MULT18X18": 48, "PLL": 2,
        })

    def test_unavailable_probe_keeps_whole_gpu_resources_unknown(self):
        result = self.build()
        self.assertEqual(result["arithmetic_probe"]["status"], "unavailable")
        self.assertIsNone(result["arithmetic_probe"]["utilization"])
        self.assert_incomplete(result)

    def test_optional_core_evidence_never_promotes_whole_board_fit(self):
        result = self.build(core={"whole_gpu_fit_qualified": True, "fits": True,
                                  "synthesis_cells": {"LUT4": 1}})
        self.assertEqual(result["logical_core"]["status"], "invalid")
        self.assertIsNone(result["logical_core"]["synthesis_cells"])
        self.assert_incomplete(result)

    def test_current_inventory_records_core_and_separate_tmds_rtl(self):
        coverage = json.loads((ROOT / "rtl/full-gpu-manifest.json").read_text())
        result = self.build(coverage=coverage)
        rows = {row["id"]: row for row in result["implementation_blocks"]}
        for name in ("coverage_interpolation", "fragment_depth_shading",
                     "texture_cache_addressing", "tile_storage_export_dma",
                     "host_parallel_link", "command_execution",
                     "primitive_bin_fetch_prefetch", "sdram_controller_arbitration",
                     "scanout_fifo", "two_dimensional_copy", "native_storage_primitives",
                     "status_recovery_control", "hdmi_output_resources"):
            with self.subTest(block=name):
                self.assertTrue(rows[name]["rtl_source_present"])
        self.assertIn("board_clock_reset_cdc", result["missing_blocks"])
        self.assertIn("production_top_pin_integration", result["missing_blocks"])
        self.assertIsNone(coverage["production_top"])
        self.assertIsNone(coverage["constraints"])
        self.assert_incomplete(result)

    def test_qualified_probe_stays_in_its_submodule_scope(self):
        result = self.build(probe=fresh_probe())
        self.assertEqual(result["arithmetic_probe"]["status"], "qualified_submodule")
        self.assertEqual(result["arithmetic_probe"]["utilization"]["LUT4"]["used"], 8718)
        self.assertEqual(result["arithmetic_probe"]["utilization"]["DFF"]["used"], 4472)
        self.assertIsNone(result["known_allocations"]["LUT4"])
        self.assertIsNone(result["known_allocations"]["DFF"])
        self.assert_incomplete(result)

    def test_forged_probe_success_flags_cannot_promote_whole_gpu(self):
        probe = fresh_probe()
        probe.update(whole_gpu_validated=True, target_met=True, fits=True,
                     scope="complete GPU", routed=True, synthesized=True)
        for clock in probe["fmax"].values():
            clock["achieved"] = 1000
        result = self.build(probe=probe)
        self.assert_incomplete(result)
        self.assertIsNone(result["known_allocations"]["LUT4"])

    def test_stale_source_hash_does_not_expose_counts_as_current(self):
        probe = fresh_probe()
        probe["source_sha256"]["rtl/uv_pipeline.v"] = "0" * 64
        result = self.build(probe=probe)
        self.assertEqual(result["arithmetic_probe"]["status"], "stale")
        self.assertIsNone(result["arithmetic_probe"]["utilization"])
        self.assertTrue(result["arithmetic_probe"]["source_mismatches"])
        self.assertEqual(result["arithmetic_probe"]["recorded_utilization"]["LUT4"]["used"], 8718)
        self.assert_incomplete(result)

    def test_missing_hashed_source_is_stale(self):
        probe = fresh_probe()
        probe["source_sha256"]["rtl/not-an-implemented-gpu.v"] = "0" * 64
        result = self.build(probe=probe)
        self.assertEqual(result["arithmetic_probe"]["status"], "stale")
        self.assertIsNone(result["arithmetic_probe"]["utilization"])
        self.assert_incomplete(result)

    def test_manifest_labels_and_probe_top_cannot_self_certify_coverage(self):
        coverage = manifest()
        for block in coverage["blocks"]:
            block["status"] = "rtl_submodule"
            block["rtl_sources"] = ["rtl/uv_pipeline.v"]
        coverage["production_top"] = "nano_uv_probe"
        coverage["constraints"] = "rtl/uv_probe.cst"
        coverage["required_clocks"] = {"core": 100800000}
        self.assert_incomplete(self.build(coverage=coverage, probe=fresh_probe()))

    def test_missing_provenance_is_not_a_current_qualified_probe(self):
        probe = fresh_probe()
        probe["source_sha256"] = {}
        result = self.build(probe=probe)
        self.assertNotEqual(result["arithmetic_probe"]["status"], "qualified_submodule")
        self.assertIsNone(result["arithmetic_probe"]["utilization"])
        self.assert_incomplete(result)

    def test_rom_and_generator_hashes_are_mandatory(self):
        for name in ("rtl/generated/reciprocal_seed_0.hex", "scripts/rtl-generate-rom.py",
                     "reference/tests/uv_oracle.hpp"):
            with self.subTest(source=name):
                probe = fresh_probe()
                del probe["source_sha256"][name]
                result = self.build(probe=probe)
                self.assertEqual(result["arithmetic_probe"]["status"], "invalid")
                self.assertIsNone(result["arithmetic_probe"]["utilization"])

    def test_changed_seed_rom_is_stale(self):
        probe = fresh_probe()
        probe["source_sha256"]["rtl/generated/reciprocal_seed_0.hex"] = "0" * 64
        result = self.build(probe=probe)
        self.assertEqual(result["arithmetic_probe"]["status"], "stale")
        self.assertIsNone(result["arithmetic_probe"]["utilization"])

    def test_generated_artifact_provenance_is_mandatory(self):
        probe = fresh_probe()
        probe.pop("generated_sha256")
        result = self.build(probe=probe)
        self.assertEqual(result["arithmetic_probe"]["status"], "invalid")
        self.assertIsNone(result["arithmetic_probe"]["utilization"])

    def test_different_device_probe_is_invalid_for_this_part(self):
        for key, value in (("device", "GW2AR-LV55PG484C8/I7"), ("family", "GW2A-55")):
            with self.subTest(field=key):
                probe = fresh_probe()
                probe[key] = value
                result = self.build(probe=probe)
                self.assertEqual(result["arithmetic_probe"]["status"], "invalid")
                self.assertIsNone(result["arithmetic_probe"]["utilization"])
                self.assert_incomplete(result)

    def test_family_iob_capacity_is_not_a_package_pin_budget(self):
        result = self.build(probe=fresh_probe())
        self.assertEqual(result["arithmetic_probe"]["utilization"]["IOB"]["available"], 384)
        self.assertIsNone(result["known_allocations"]["IOB"])
        self.assertNotIn("IOB", result["device"]["capacity"])

    def test_report_does_not_mutate_input_evidence(self):
        model, coverage, probe = profile(), manifest(), fresh_probe()
        expected = copy.deepcopy((model, coverage, probe))
        resources.build_report(ROOT, model, coverage, probe)
        self.assertEqual((model, coverage, probe), expected)

    def test_cli_require_full_fit_writes_report_and_fails_closed(self):
        with tempfile.TemporaryDirectory(prefix="nano-fit-report-test-") as directory:
            directory = Path(directory)
            model_path = directory / "model.json"
            manifest_path = directory / "manifest.json"
            probe_path = directory / "probe.json"
            output_path = directory / "report.json"
            for path, data in ((model_path, profile()), (manifest_path, manifest()),
                               (probe_path, fresh_probe())):
                path.write_text(json.dumps(data), encoding="utf-8")
            result = subprocess.run([
                sys.executable, str(MODULE), "--model-profile", str(model_path),
                "--manifest", str(manifest_path), "--probe-report", str(probe_path),
                "--core-report", str(directory / "absent-core.json"),
                "--output", str(output_path), "--require-full-fit",
            ], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertTrue(output_path.is_file(), result.stdout + result.stderr)
            self.assert_incomplete(json.loads(output_path.read_text(encoding="utf-8")))


class LogicalCoreEvidenceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="nano-core-evidence-test-")
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        sources = resources.CORE_REQUIRED_INPUTS | {
            "reference/include/nano_gpu/reference.hpp", "build/rtl-gpu/seed_rom.v",
        }
        for name in sources:
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture input: " + name + "\n", encoding="utf-8")
        self.report_path = self.repo / "build/rtl-gpu/build-report.json"
        self.report = {
            "schema": "N20K-GPU-RTL-BUILD-1", "top": "nano_gpu_core",
            "device": resources.PART, "family": resources.FAMILY,
            "simulation_backend": "verilator", "verilator_version": "Verilator fixture",
            "cpp_device_backends_linked": False, "whole_gpu_fit_qualified": False,
            "source_sha256": {name: resources.digest(self.repo / name) for name in sources},
        }
        self.report_path.write_text(json.dumps(self.report), encoding="utf-8")

    def summary(self):
        return resources.logical_core_summary(self.repo, self.report, self.report_path)

    def add_synthesis(self):
        path = self.report_path.parent / "gpu-synth.json"
        path.write_text(json.dumps({"modules": {"nano_gpu_core": {"cells": {
            "lookup": {"type": "LUT4"}, "reg0": {"type": "DFF"},
            "reg1": {"type": "DFF"}, "memory": {"type": "SDPB"},
        }}}}), encoding="utf-8")
        self.report.update(synthesis_cells={"LUT4": 1, "DFF": 2, "SDPB": 1},
                           yosys_version="Yosys fixture", netlist_sha256=resources.digest(path))
        return path

    def test_missing_build_report_is_unavailable(self):
        result = resources.logical_core_summary(self.repo, None)
        self.assertEqual(result["status"], "unavailable")
        self.assertIsNone(result["synthesis_cells"])

    def test_source_matched_verilator_build_is_not_simulation_or_fit_qualification(self):
        self.report.update(simulation_passed=True, fits=True, target_met=True,
                           scope="complete production GPU")
        result = self.summary()
        self.assertEqual(result["status"], "source_matched_build")
        self.assertIsNone(result["simulation_passed"])
        self.assertIsNone(result["timing_met"])
        self.assertFalse(result["whole_gpu_evidence"])
        self.assertIsNone(result["synthesis_cells"])
        self.assertEqual(result["synthesis_status"], "unavailable")

    def test_schema_top_part_backend_and_scope_flags_are_required(self):
        original = copy.deepcopy(self.report)
        for key, value in (("schema", "other"), ("top", "nano_uv_probe"),
                           ("device", "GW2AR-LV55PG484C8/I7"), ("family", "GW2A-55"),
                           ("simulation_backend", "cpp"), ("cpp_device_backends_linked", True),
                           ("whole_gpu_fit_qualified", True), ("verilator_version", "")):
            with self.subTest(field=key):
                self.report = copy.deepcopy(original)
                self.report[key] = value
                result = self.summary()
                self.assertEqual(result["status"], "invalid")
                self.assertIsNone(result["synthesis_cells"])

    def test_every_rtl_rom_generator_and_adapter_source_is_mandatory(self):
        original = copy.deepcopy(self.report)
        for name in ("rtl/gpu/nano_gpu_core.sv", "rtl/gpu/nano_tmds.sv",
                     "rtl/generated/reciprocal_seed_0.hex", "scripts/gpu-rtl.py",
                     "scripts/rtl-generate-rom.py", "reference/rtl/sdram_model.cpp",
                     "reference/include/nano_gpu/reference.hpp", "build/rtl-gpu/seed_rom.v"):
            with self.subTest(source=name):
                self.report = copy.deepcopy(original)
                del self.report["source_sha256"][name]
                result = self.summary()
                self.assertEqual(result["status"], "invalid")
                self.assertIsNone(result["synthesis_cells"])

    def test_new_gpu_modules_and_public_headers_cannot_escape_provenance(self):
        for name in ("rtl/gpu/new_module.sv", "reference/include/new_header.hpp"):
            with self.subTest(source=name):
                path = self.repo / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("new source\n", encoding="utf-8")
                self.assertEqual(self.summary()["status"], "invalid")
                path.unlink()

    def test_stale_source_blocks_current_synthesis_counts(self):
        self.add_synthesis()
        (self.repo / "rtl/gpu/nano_gpu_core.sv").write_text("changed\n", encoding="utf-8")
        result = self.summary()
        self.assertEqual(result["status"], "stale")
        self.assertIn("rtl/gpu/nano_gpu_core.sv", result["source_mismatches"])
        self.assertIsNone(result["synthesis_cells"])
        self.assertEqual(result["recorded_synthesis_cells"]["DFF"], 2)

    def test_unsafe_source_paths_and_invalid_hashes_fail_closed(self):
        original = copy.deepcopy(self.report)
        for name in ("../outside.v", "/absolute.v"):
            with self.subTest(source=name):
                self.report = copy.deepcopy(original)
                self.report["source_sha256"][name] = "0" * 64
                result = self.summary()
                self.assertEqual(result["status"], "stale")
                self.assertIn(name, result["source_mismatches"])
        self.report = original
        self.report["source_sha256"]["rtl/uv_pipeline.v"] = True
        self.assertEqual(self.summary()["status"], "invalid")

    def test_netlist_hash_and_actual_cell_counts_enable_only_scoped_counts(self):
        self.add_synthesis()
        result = self.summary()
        self.assertEqual(result["status"], "source_matched_build")
        self.assertEqual(result["synthesis_status"], "source_matched_recorded_synthesis")
        self.assertEqual(result["synthesis_cells"], {"DFF": 2, "LUT4": 1, "SDPB": 1})
        self.assertTrue(result["netlist_hash_verified"])
        self.assertTrue(result["netlist_cells_verified"])
        self.assertFalse(result["implementation_regenerated"])
        self.assertFalse(result["whole_gpu_evidence"])
        self.assertIsNone(result["timing_met"])

    def test_valid_core_counts_never_become_whole_board_or_model_totals(self):
        self.add_synthesis()
        result = resources.build_report(self.repo, profile(), manifest(),
                                        core_report=self.report, core_report_path=self.report_path)
        self.assertEqual(result["logical_core"]["synthesis_cells"]["LUT4"], 1)
        self.assertIsNone(result["known_allocations"]["LUT4"])
        self.assertIsNone(result["known_allocations"]["DFF"])
        self.assertIsNone(result["whole_gpu"]["mapped_resource_counts"])
        self.assertEqual(result["whole_gpu"]["status"], "incomplete")
        self.assertIsNone(result["whole_gpu"]["fits"])
        self.assertIsNone(result["whole_gpu"]["timing_met"])

    def test_missing_or_changed_netlist_blocks_current_counts(self):
        path = self.add_synthesis()
        path.write_text("changed\n", encoding="utf-8")
        self.assertEqual(self.summary()["synthesis_status"], "invalid")
        self.assertIsNone(self.summary()["synthesis_cells"])
        path.unlink()
        self.assertEqual(self.summary()["synthesis_status"], "invalid")

    def test_forged_recorded_totals_do_not_match_the_netlist(self):
        self.add_synthesis()
        self.report["synthesis_cells"]["LUT4"] = 0
        result = self.summary()
        self.assertEqual(result["synthesis_status"], "invalid")
        self.assertTrue(result["netlist_hash_verified"])
        self.assertFalse(result["netlist_cells_verified"])
        self.assertIsNone(result["synthesis_cells"])

    def test_missing_synthesis_provenance_and_noninteger_counts_are_invalid(self):
        for key in ("netlist_sha256", "yosys_version", "synthesis_cells"):
            with self.subTest(field=key):
                self.add_synthesis()
                del self.report[key]
                result = self.summary()
                self.assertEqual(result["synthesis_status"], "invalid")
                self.assertIsNone(result["synthesis_cells"])
        self.add_synthesis()
        self.report["synthesis_cells"]["DFF"] = True
        self.assertEqual(self.summary()["synthesis_status"], "invalid")

    def test_wrong_netlist_top_cannot_supply_core_counts(self):
        path = self.add_synthesis()
        path.write_text(json.dumps({"modules": {"nano_uv_probe": {"cells": {}}}}), encoding="utf-8")
        self.report["netlist_sha256"] = resources.digest(path)
        result = self.summary()
        self.assertEqual(result["synthesis_status"], "invalid")
        self.assertIsNone(result["synthesis_cells"])

    def test_summary_does_not_mutate_recorded_evidence(self):
        self.add_synthesis()
        original = copy.deepcopy(self.report)
        self.summary()
        self.assertEqual(self.report, original)


class CorePackEvidenceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="nano-pack-evidence-test-")
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        for name in resources.CORE_PACK_REQUIRED_INPUTS:
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture: " + name, encoding="utf-8")
        used = {"LUT4": 29300, "DFF": 13926, "ALU": 9442,
                "MULT18X18": 43, "BSRAM": 25, "RAM16SDP4": 20}
        self.report = {
            "schema": "N20K-CORE-PACK-PROBE-1", "top": "nano_gpu_probe",
            "device": resources.PART, "family": resources.FAMILY,
            "scope": resources.CORE_PACK_SCOPE, **resources.CORE_PACK_OBSERVABILITY,
            "whole_gpu_fit_qualified": False, "board_pinout_qualified": False,
            "production_pll_tmds_integrated": False, "routed": False, "timing_qualified": False,
            "elaboration_passed": True, "synthesized": True, "packed": True,
            "synthesis_returncode": 0, "pack_returncode": 0,
            "inputs_changed_during_run": [], "packer_errors": [],
            "generated_rom_sha256": "0" * 64, "netlist_sha256": "1" * 64,
            "packed_netlist_sha256": "2" * 64,
            "source_sha256": {name: resources.digest(self.repo / name)
                              for name in resources.CORE_PACK_REQUIRED_INPUTS},
            "packer_utilization": {name: {"used": used[name], "available": capacity,
                                         "within_capacity": True}
                                   for name, capacity in resources.CORE_PACK_CAPACITIES.items()},
            "reported_resources_within_capacity": True, "capacity_rejected_resources": [],
        }
        self.report["packer_utilization"]["IOB"] = {"used": 9, "available": 384,
                                                     "within_capacity": True}

    def summary(self, build_path=None):
        return resources.core_pack_summary(self.repo, self.report, build_path)

    def test_actual_probe_overage_is_recomputed_despite_success_labels(self):
        result = self.summary()
        self.assertEqual(result["status"], "capacity_rejected")
        self.assertTrue(result["source_matched"])
        self.assertFalse(result["resource_capacity_met"])
        self.assertEqual(result["capacity_overages"], {"LUT4": 8564})
        self.assertEqual(result["capacity_rejected_resources"], ["LUT4"])
        self.assertEqual(result["utilization"]["ALU"]["used"], 9442)
        self.assertEqual(result["utilization"]["LUT4"]["used"], 29300)
        self.assertNotIn("IOB", result["utilization"])
        self.assertFalse(result["generated_artifacts_revalidated"])
        self.assertFalse(result["whole_gpu_evidence"])
        self.assertIsNone(result["timing_met"])

    def test_whole_gpu_gets_a_concrete_probe_capacity_blocker_without_fit_claim(self):
        result = resources.build_report(self.repo, profile(), manifest(), core_pack_report=self.report)
        blockers = result["whole_gpu"]["blockers"]
        self.assertTrue(any("LUT4 capacity: 29300 / 20736 (+8564; includes probe overhead)" in reason
                            for reason in blockers))
        self.assertIsNone(result["whole_gpu"]["fits"])
        self.assertIsNone(result["whole_gpu"]["timing_met"])
        self.assertIsNone(result["whole_gpu"]["mapped_resource_counts"])
        self.assertIsNone(result["known_allocations"]["LUT4"])

    def test_probe_scope_and_complete_observation_metadata_are_required(self):
        original = copy.deepcopy(self.report)
        bad = {"device": "GW2AR-LV55PG484C8/I7", "family": "GW2A-55",
               "schema": "other", "top": "nano_uv_probe", "scope": "complete board",
               "probe_input_shift_bits": 49, "probe_output_snapshot_bits": 115,
               "probe_checksum_bits": 1, "probe_maximum_register_overhead": 0,
               "whole_gpu_fit_qualified": True, "board_pinout_qualified": True,
               "production_pll_tmds_integrated": True}
        for key, value in bad.items():
            with self.subTest(field=key):
                self.report = copy.deepcopy(original)
                self.report[key] = value
                self.assertEqual(self.summary()["status"], "invalid")

    def test_missing_or_stale_provenance_cannot_reject_current_core_capacity(self):
        original = copy.deepcopy(self.report)
        for name in ("rtl/gpu_probe.sv", "rtl/gpu_probe.cst", "rtl/gpu/nano_gpu_core.sv",
                     "scripts/rtl-core-pack.py", "scripts/rtl-generate-rom.py",
                     "rtl/generated/reciprocal_seed_0.hex"):
            with self.subTest(source=name):
                self.report = copy.deepcopy(original)
                del self.report["source_sha256"][name]
                self.assertEqual(self.summary()["status"], "invalid")
        self.report = copy.deepcopy(original)
        self.report["source_sha256"]["rtl/gpu/nano_gpu_core.sv"] = "0" * 64
        self.assertEqual(self.summary()["status"], "stale")
        result = resources.build_report(self.repo, profile(), manifest(), core_pack_report=self.report)
        self.assertFalse(any("serial probe exceeds" in reason for reason in result["whole_gpu"]["blockers"]))
        for key in ("generated_rom_sha256", "netlist_sha256", "packed_netlist_sha256"):
            with self.subTest(field=key):
                self.report = copy.deepcopy(original)
                del self.report[key]
                self.assertEqual(self.summary()["status"], "invalid")

    def test_counts_and_available_capacities_cannot_be_forged_by_type_or_label(self):
        original = copy.deepcopy(self.report)
        for field, value in (("used", -1), ("used", True), ("used", 1.5),
                             ("available", 999999), ("available", True)):
            with self.subTest(field=field, value=value):
                self.report = copy.deepcopy(original)
                self.report["packer_utilization"]["LUT4"][field] = value
                result = self.summary()
                self.assertEqual(result["status"], "invalid")
                self.assertIsNone(result["utilization"])
        self.report = original
        self.report["packer_utilization"]["LUT4"]["used"] = 20736
        self.report["packer_utilization"]["LUT4"]["within_capacity"] = False
        self.report["reported_resources_within_capacity"] = False
        self.report["capacity_rejected_resources"] = ["LUT4"]
        result = self.summary()
        self.assertTrue(result["resource_capacity_met"])
        self.assertEqual(result["capacity_overages"], {})
        self.assertFalse(result["whole_gpu_evidence"])

    def test_unstable_inputs_and_tool_failures_do_not_qualify_counts(self):
        original = copy.deepcopy(self.report)
        for key, value in (("inputs_changed_during_run", ["rtl/gpu/nano_gpu_core.sv"]),
                           ("elaboration_passed", False), ("synthesized", False),
                           ("packed", False), ("synthesis_returncode", 1),
                           ("pack_returncode", True), ("packer_errors", ["failure"])):
            with self.subTest(field=key):
                self.report = copy.deepcopy(original)
                self.report[key] = value
                result = self.summary()
                self.assertEqual(result["status"], "invalid")
                self.assertIsNone(result["utilization"])

    def test_retained_generated_artifacts_are_verified_when_requested(self):
        directory = self.repo / "build/rtl-core-pack"
        directory.mkdir(parents=True)
        (directory / "seed_rom.v").write_text("generated ROM", encoding="utf-8")
        self.report["generated_rom_sha256"] = resources.digest(directory / "seed_rom.v")
        inventories = {"synthesis_cells": {"LUT4": 1}, "packed_cells": {"LUT4": 1}}
        for filename, field, top, hash_key in (
                ("gpu-probe-synth.json", "synthesis_cells", "nano_gpu_probe", "netlist_sha256"),
                ("gpu-probe-packed.json", "packed_cells", "top", "packed_netlist_sha256")):
            path = directory / filename
            path.write_text(json.dumps({"modules": {top: {"cells": {"lookup": {"type": "LUT4"}}}}}),
                            encoding="utf-8")
            self.report[hash_key] = resources.digest(path)
        for name, row in self.report["packer_utilization"].items():
            if name in resources.CORE_PACK_CAPACITIES:
                row["used"] = 1 if name == "LUT4" else 0
        build = {**copy.deepcopy(self.report), **inventories}
        path = directory / "qualification.json"
        path.write_text(json.dumps(build), encoding="utf-8")
        result = self.summary(path)
        self.assertTrue(result["generated_artifacts_revalidated"])
        self.assertFalse(result["implementation_regenerated"])
        self.assertTrue(result["resource_capacity_met"])
        (directory / "gpu-probe-packed.json").write_text("changed", encoding="utf-8")
        result = self.summary(path)
        self.assertEqual(result["status"], "invalid")
        self.assertFalse(result["generated_artifacts_revalidated"])
        self.assertIsNone(result["utilization"])

    def test_pack_summary_preserves_input_report(self):
        original = copy.deepcopy(self.report)
        self.summary()
        self.assertEqual(self.report, original)


if __name__ == "__main__":
    unittest.main()
