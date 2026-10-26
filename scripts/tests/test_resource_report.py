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
    def build(self, model=None, coverage=None, probe=None):
        return resources.build_report(ROOT, model or profile(), coverage or manifest(), probe)

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
                "--output", str(output_path), "--require-full-fit",
            ], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertTrue(output_path.is_file(), result.stdout + result.stderr)
            self.assert_incomplete(json.loads(output_path.read_text(encoding="utf-8")))


if __name__ == "__main__":
    unittest.main()
