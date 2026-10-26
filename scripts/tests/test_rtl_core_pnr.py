"""Fail-closed source proof and independent-clock checks for the GPU probe runner."""

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("rtl_core_pnr", ROOT / "scripts/rtl-core-pnr.py")
pnr = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pnr)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def synth_design():
    return {"modules": {"nano_gpu_probe": {
        "attributes": {"top": "1"},
        "ports": {"clk": {"direction": "input", "bits": [2]},
                  "host_clk": {"direction": "input", "bits": [3]}},
        "netnames": {"clk": {"bits": [2]}, "host_clk": {"bits": [3]},
                     "core.clk": {"bits": [4]}, "core.host_clk": {"bits": [5]},
                     "core_outputs": {"bits": [10] * 105 + [4] + [10] * 10}},
        "cells": {"core_clock_input": {"type": "IBUF", "connections": {"I": [2], "O": [4]},
                                       "port_directions": {"I": "input", "O": "output"}},
                  "host_clock_input": {"type": "IBUF", "connections": {"I": [3], "O": [5]},
                                       "port_directions": {"I": "input", "O": "output"}}}}}}


def placed_cell(kind, tile=0, index=0):
    if kind.startswith("LUT") or kind == "BLOCKER_LUT":
        location = "LUT" + str(index)
    elif kind.startswith("DFF") or kind == "BLOCKER_FF":
        location = "DFF" + str(index)
    elif kind == "ALU":
        location = "ALU" + str(index)
    elif kind == "RAM16SDP4":
        location = "RAM16SDP4"
    elif kind == "MULT18X18":
        location = "MULT18X18_" + str(index)
    else:
        location = "BSRAM"
    return {"type": kind, "attributes": {"NEXTPNR_BEL": "X{}Y1/{}".format(tile, location)}}


def routed_cells():
    kinds = ("LUT4", "LUT2", "BLOCKER_LUT", "DFFE", "BLOCKER_FF",
             "SDP", "SP", "DP", "ROM", "MULT18X18")
    return {"physical_" + kind: placed_cell(kind, tile) for tile, kind in enumerate(kinds)}


def good_timing(core=110.0, host=28.0):
    return {"fmax": {"core_outputs[105]": {"achieved": core, "constraint": 100.8},
                     "core.host_clk": {"achieved": host, "constraint": 25.2}},
            "critical_paths": []}


class ProbeFixture(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.pack = self.root / "build/rtl-core-pack"
        self.build = self.root / "build/rtl-core-pnr"
        self.report = self.root / "docs/hardware/rtl-core-pnr.json"
        self.pack.mkdir(parents=True)
        names = ("rtl/uv_mult.v", "rtl/uv_pipeline.v", "rtl/gpu/core.sv", "rtl/gpu_probe.sv",
                 "rtl/gpu_probe.cst", "scripts/rtl-core-pack.py", "scripts/rtl-generate-rom.py",
                 "scripts/rtl-check.py", "rtl/generated/seed.hex")
        for name in names:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture input " + name + "\n", encoding="utf-8")
        self.netlist = self.pack / "gpu-probe-synth.json"
        self.netlist.write_text(json.dumps(synth_design()), encoding="utf-8")
        self.packed = self.pack / "gpu-probe-packed.json"
        self.packed.write_text(json.dumps({"modules": {"top": {
            "settings": {"pack": "1", "place": "0", "route": "0"},
            "cells": {"initial_lut": {"type": "LUT4"}}}}}), encoding="utf-8")
        self.rom = self.pack / "seed_rom.v"
        self.rom.write_text("module seed; endmodule\n", encoding="utf-8")
        self.proof = {"schema": "N20K-CORE-PACK-PROBE-1", "top": "nano_gpu_probe",
                      "device": "GW2AR-LV18QN88C8/I7", "family": "GW2A-18C",
                      "elaboration_passed": True, "synthesized": True, "packed": True,
                      "synthesis_returncode": 0, "pack_returncode": 0,
                      "inputs_changed_during_run": [], "packer_errors": [], "reported_resources_within_capacity": True,
                      "packer_utilization": {name: {"used": 0, "available": capacity, "within_capacity": True}
                                             for name, capacity in (("LUT4", 20736), ("DFF", 15552),
                                                                    ("BSRAM", 46), ("MULT18X18", 48))},
                      "physical_lut_lower_bound": {"within_capacity": True,
                                                   "minimum_physical_lut_sites": 0,
                                                   "available_lut_sites": 20736},
                      "source_sha256": {name: digest(self.root / name) for name in names},
                      "netlist_sha256": digest(self.netlist), "generated_rom_sha256": digest(self.rom),
                      "packed_netlist_sha256": digest(self.packed)}
        self.proof["packer_utilization"]["LUT4"]["used"] = 1
        self.proof["physical_lut_lower_bound"]["minimum_physical_lut_sites"] = 1
        self.save_proof()

    def save_proof(self):
        (self.pack / "qualification.json").write_text(json.dumps(self.proof), encoding="utf-8")

    def validate(self):
        return pnr.validate_pack(self.root, self.pack)


class PackProofTests(ProbeFixture):
    def test_current_pack_returns_clock_aliases_across_input_buffers(self):
        proof, aliases = self.validate()
        self.assertEqual(proof["netlist_sha256"], digest(self.netlist))
        self.assertTrue({"clk", "core.clk", "core_outputs[105]"}.issubset(aliases["clk"]))
        self.assertTrue({"host_clk", "core.host_clk"}.issubset(aliases["host_clk"]))
        self.assertTrue(set(aliases["clk"]).isdisjoint(aliases["host_clk"]))

    def test_stale_source_synthesis_rom_and_packed_artifact_are_rejected(self):
        for path in (self.root / "rtl/uv_pipeline.v", self.netlist, self.rom, self.packed):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b"changed\n")
                with self.assertRaises((RuntimeError, ValueError)):
                    self.validate()
                path.write_bytes(original)

    def test_missing_required_source_cannot_be_hidden_by_short_manifest(self):
        del self.proof["source_sha256"]["rtl/uv_pipeline.v"]
        self.save_proof()
        with self.assertRaises((RuntimeError, ValueError)):
            self.validate()

    def test_new_gpu_source_requires_repacking(self):
        (self.root / "rtl/gpu/new_block.sv").write_text("module added; endmodule\n")
        with self.assertRaises((RuntimeError, ValueError)):
            self.validate()

    def test_invalid_part_profile_and_failed_pack_flags_are_rejected(self):
        mutations = (("device", "GW1N-9"), ("family", "GW1N-9"), ("top", "other"),
                     ("schema", "old"), ("packed", False), ("synthesized", False),
                     ("elaboration_passed", False), ("pack_returncode", 125),
                     ("synthesis_returncode", 1), ("inputs_changed_during_run", ["rtl/uv_pipeline.v"]),
                     ("reported_resources_within_capacity", False))
        original = copy.deepcopy(self.proof)
        for key, value in mutations:
            with self.subTest(field=key):
                self.proof = copy.deepcopy(original)
                self.proof[key] = value
                self.save_proof()
                with self.assertRaises((RuntimeError, ValueError)):
                    self.validate()

    def test_capacity_claim_cannot_override_overfull_resource_or_lut_bound(self):
        self.proof["packer_utilization"]["LUT4"]["used"] = 20737
        self.save_proof()
        with self.assertRaises((RuntimeError, ValueError)):
            self.validate()
        self.proof["packer_utilization"]["LUT4"]["used"] = 0
        self.proof["physical_lut_lower_bound"]["within_capacity"] = False
        self.save_proof()
        with self.assertRaises((RuntimeError, ValueError)):
            self.validate()

    def test_source_manifest_cannot_read_outside_repository(self):
        self.proof["source_sha256"]["../outside"] = "0" * 64
        self.save_proof()
        with self.assertRaises((RuntimeError, ValueError)):
            self.validate()


class TimingTests(unittest.TestCase):
    def setUp(self):
        self.aliases = pnr.clock_aliases(synth_design())

    def test_both_independent_clock_targets_must_pass(self):
        self.assertTrue(pnr.validate_timing(good_timing(), self.aliases)["target_met"])
        self.assertFalse(pnr.validate_timing(good_timing(core=99.0), self.aliases)["target_met"])
        self.assertFalse(pnr.validate_timing(good_timing(host=24.0), self.aliases)["target_met"])

    def test_actual_integer_picosecond_quantized_targets_are_valid_but_core_still_misses(self):
        timing = good_timing(core=48, host=78)
        timing["fmax"]["core_outputs[105]"]["constraint"] = 100.80644989013672
        timing["fmax"]["core.host_clk"]["constraint"] = 25.20034408569336
        result = pnr.validate_timing(timing, self.aliases)
        self.assertFalse(result["target_met"])
        self.assertFalse(result["clocks"]["clk"]["target_met"])
        self.assertTrue(result["clocks"]["host_clk"]["target_met"])
        self.assertEqual(result["clocks"]["clk"]["constraint"], 100.80644989013672)
        self.assertEqual(result["clocks"]["clk"]["requested_mhz"], 100.8)
        self.assertEqual(result["clocks"]["host_clk"]["requested_mhz"], 25.2)

    def test_constraint_period_difference_larger_than_one_picosecond_is_rejected(self):
        for clock, target in (("core_outputs[105]", 100.8), ("core.host_clk", 25.2)):
            for difference in (-1.25, 1.25):
                with self.subTest(clock=clock, period_difference=difference):
                    timing = good_timing()
                    timing["fmax"][clock]["constraint"] = 1e6 / (1e6 / target + difference)
                    with self.assertRaises((RuntimeError, ValueError)):
                        pnr.validate_timing(timing, self.aliases)

    def test_looser_quantized_constraint_cannot_relax_requested_frequency(self):
        for clock, target in (("core_outputs[105]", 100.8), ("core.host_clk", 25.2)):
            with self.subTest(clock=clock):
                timing = good_timing()
                reported = 1e6 / (1e6 / target + 0.75)
                timing["fmax"][clock]["constraint"] = reported
                timing["fmax"][clock]["achieved"] = (reported + target) / 2
                result = pnr.validate_timing(timing, self.aliases)
                self.assertFalse(result["target_met"])

    def test_one_picosecond_quantization_boundary_is_inclusive(self):
        for difference in (-1.0, 1.0):
            with self.subTest(period_difference=difference):
                timing = good_timing()
                timing["fmax"]["core_outputs[105]"]["constraint"] = 1e6 / (1e6 / 100.8 + difference)
                self.assertTrue(pnr.validate_timing(timing, self.aliases)["target_met"])

    def test_missing_extra_nonfinite_and_incorrect_clock_targets_are_rejected(self):
        cases = []
        timing = good_timing(); del timing["fmax"]["core.host_clk"]; cases.append(timing)
        timing = good_timing(); timing["fmax"]["core.host_clk"]["constraint"] = 100.8; cases.append(timing)
        timing = good_timing(); timing["fmax"]["unknown"] = {"achieved": 110, "constraint": 100.8}; cases.append(timing)
        for value in (float("nan"), float("inf"), -1, True):
            timing = good_timing(); timing["fmax"]["core_outputs[105]"]["achieved"] = value; cases.append(timing)
        for value in (None, [], "malformed"):
            timing = good_timing(); timing["fmax"]["core_outputs[105]"] = value; cases.append(timing)
        for timing in cases:
            with self.subTest(timing=timing):
                with self.assertRaises((RuntimeError, ValueError)):
                    pnr.validate_timing(timing, self.aliases)


class PhysicalResourceTests(unittest.TestCase):
    def test_count_physical_sites_including_alu_and_distributed_ram_reservations(self):
        cells = routed_cells()
        cells["carry"] = placed_cell("ALU", 100)
        cells["carry_blocker"] = placed_cell("BLOCKER_LUT", 100)
        cells["carry_register"] = placed_cell("DFFE", 100)
        cells["distributed_ram"] = placed_cell("RAM16SDP4", 101)
        result = pnr.physical_resources(cells)
        self.assertEqual(set(result), {"LUT4", "DFF", "BSRAM", "MULT18X18"})
        for name, used in (("LUT4", 12), ("DFF", 9), ("BSRAM", 4), ("MULT18X18", 1)):
            self.assertEqual(result[name]["used"], used)
            self.assertTrue(result[name]["within_capacity"])

    def test_stripped_blockers_do_not_reduce_reserved_physical_footprint(self):
        cells = {"ram": placed_cell("RAM16SDP4", 0), "carry": placed_cell("ALU", 1),
                 "carry_ff": placed_cell("DFFE", 1), "pass_lut": placed_cell("LUT4", 2),
                 "pass_ff": placed_cell("DFFE", 2), "carry_blocker": placed_cell("BLOCKER_LUT", 1)}
        cells.update({"ram_storage_" + str(i): placed_cell("LUT4", 0, i) for i in range(4)})
        cells.update({"ram_lut_blocker_" + str(i): placed_cell("BLOCKER_LUT", 0, i) for i in range(4, 8)})
        cells.update({"ram_ff_blocker_" + str(i): placed_cell("BLOCKER_FF", 0, i) for i in range(6)})
        with_blockers = pnr.physical_resources(cells)
        stripped = {name: cell for name, cell in cells.items() if not cell["type"].startswith("BLOCKER_")}
        self.assertEqual(pnr.physical_resources(stripped), with_blockers)
        self.assertEqual(with_blockers["LUT4"]["used"], 10)
        self.assertEqual(with_blockers["DFF"]["used"], 8)

    def test_synthesis_ram_aliases_and_all_lut_sizes_are_counted(self):
        kinds = ("LUT1", "LUT2", "LUT3", "LUT4", "SDPB", "SPX9", "DPX9", "SDPX9", "ROMX9", "pROM", "pROMX9")
        result = pnr.physical_resources({kind: placed_cell(kind, i) for i, kind in enumerate(kinds)})
        self.assertEqual(result["LUT4"]["used"], 4)
        self.assertEqual(result["BSRAM"]["used"], 7)

    def test_each_capacity_boundary_is_enforced(self):
        for name, kind, capacity in (("LUT4", "LUT4", 20736), ("DFF", "DFFE", 15552),
                                     ("BSRAM", "SDP", 46), ("MULT18X18", "MULT18X18", 48)):
            with self.subTest(resource=name):
                cells = {str(index): placed_cell(kind, index) for index in range(capacity)}
                self.assertTrue(pnr.physical_resources(cells)[name]["within_capacity"])
                cells["excess"] = placed_cell(kind, capacity)
                result = pnr.physical_resources(cells)
                self.assertEqual(result[name]["used"], capacity + 1)
                self.assertFalse(result[name]["within_capacity"])

    def test_missing_or_malformed_physical_cells_and_locations_are_rejected(self):
        for cells in ({}, None, [], {"bad": {}}, {"bad": {"type": None}},
                      {"bad": {"type": "LUT4"}},
                      {"bad": {"type": "DFFE", "attributes": {"NEXTPNR_BEL": "invalid"}}}):
            with self.subTest(cells=cells):
                with self.assertRaises((RuntimeError, ValueError)):
                    pnr.physical_resources(cells)


class HeartbeatTests(unittest.TestCase):
    def test_periodic_heartbeat_runs_during_native_wait_and_stops_on_return_or_error(self):
        real_thread = threading.Thread
        for native_error in (False, True):
            with self.subTest(native_error=native_error), tempfile.TemporaryDirectory() as directory:
                heartbeat_seen = threading.Event()
                monitors = []

                class Capture(io.StringIO):
                    def write(self, text):
                        count = super().write(text)
                        if "PnR still running" in text:
                            heartbeat_seen.set()
                        return count

                def record_thread(*args, **kwargs):
                    thread = real_thread(*args, **kwargs)
                    monitors.append(thread)
                    return thread

                def native_wait(command, **kwargs):
                    if not heartbeat_seen.wait(2):
                        raise AssertionError("No heartbeat while the native process was blocked")
                    if native_error:
                        raise RuntimeError("native process error")
                    return subprocess.CompletedProcess(command, 0)

                capture = Capture()
                logfile = Path(directory) / "pnr.log"
                with mock.patch.object(pnr.threading, "Thread", side_effect=record_thread), \
                     mock.patch.object(pnr.subprocess, "run", side_effect=native_wait), \
                     contextlib.redirect_stdout(capture):
                    if native_error:
                        with self.assertRaisesRegex(RuntimeError, "native process error"):
                            pnr.run_pnr(["fake-nextpnr"], {}, logfile, heartbeat_seconds=0.005)
                    else:
                        result = pnr.run_pnr(["fake-nextpnr"], {}, logfile, heartbeat_seconds=0.005)
                        self.assertEqual(result.returncode, 0)
                self.assertTrue(heartbeat_seen.is_set())
                self.assertIn(str(logfile), capture.getvalue())
                self.assertTrue(logfile.exists())
                self.assertTrue(monitors)
                self.assertTrue(all(not monitor.is_alive() for monitor in monitors))


class RunnerTests(ProbeFixture):
    def invoke(self, *, require_timing=False, timing=None, mutate=False, mutate_snapshot=False,
               log_host=True, route_settings=None, physical_cells=None):
        native_calls = []
        def fake_run(command, **kwargs):
            native_calls.append(command)
            if "--version" in command:
                return subprocess.CompletedProcess(command, 0, stdout="nextpnr test version\n")
            log = kwargs["stdout"]
            log.write("Info: constraining clock net 'core_outputs[105]' to 100.80 MHz\n")
            if log_host:
                log.write("Info: constraining clock net 'core.host_clk' to 25.20 MHz\n")
            log.write("Info: Routing..\nInfo: Routing complete.\nInfo: Program finished normally.\n")
            output = Path(command[command.index("--write") + 1])
            output.write_text(json.dumps({"modules": {"top": {
                "settings": {"place": "1", "route": "1"} if route_settings is None else route_settings,
                "cells": routed_cells() if physical_cells is None else physical_cells, "netnames": {}}}}))
            Path(command[command.index("--report") + 1]).write_text(json.dumps(good_timing() if timing is None else timing))
            if mutate_snapshot:
                Path(command[command.index("--json") + 1]).write_text("changed copied netlist\n")
            if mutate:
                (self.root / "rtl/uv_pipeline.v").write_text("changed while routing\n")
            return subprocess.CompletedProcess(command, 0)
        args = ["--pack-dir", str(self.pack), "--build-dir", str(self.build), "--report", str(self.report)]
        if require_timing:
            args.append("--require-timing")
        with mock.patch.object(pnr, "ROOT", self.root), \
             mock.patch.object(pnr.HDL, "tool", return_value=["fake-nextpnr"]), \
             mock.patch.object(pnr.HDL, "environment", return_value={}), \
             mock.patch.object(pnr.subprocess, "run", side_effect=fake_run), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = pnr.main(args)
        return code, json.loads(self.report.read_text()) if self.report.exists() else {}, native_calls

    def analyze_existing(self, *, require_timing=False):
        args = ["--pack-dir", str(self.pack), "--build-dir", str(self.build),
                "--report", str(self.report), "--analyze-existing"]
        if require_timing:
            args.append("--require-timing")
        with mock.patch.object(pnr, "ROOT", self.root), \
             mock.patch.object(pnr.HDL, "tool", return_value=["fake-nextpnr"]), \
             mock.patch.object(pnr.HDL, "environment", return_value={}), \
             mock.patch.object(pnr.subprocess, "run", side_effect=AssertionError("Recovery must not run a native tool")) as native, \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            code = pnr.main(args)
        self.assertEqual(native.call_count, 0)
        return code, json.loads(self.report.read_text())

    def refresh_retained_hash(self, name):
        path = self.build / "retained-evidence.json"
        manifest = json.loads(path.read_text())
        manifest["sha256"][name] = digest(self.build / name)
        path.write_text(json.dumps(manifest))

    def test_analyze_existing_preserves_producer_identity_without_running_tools(self):
        timing = good_timing(core=48, host=78)
        timing["fmax"]["core_outputs[105]"]["constraint"] = 100.80644989013672
        timing["fmax"]["core.host_clk"]["constraint"] = 25.20034408569336
        code, _, _ = self.invoke(timing=timing)
        self.assertEqual(code, 0)
        producer = json.loads((self.build / "producer-qualification.json").read_text())
        producer_script = self.build / "producer-runner.py"
        producer_script.write_bytes(producer_script.read_bytes() + b"\n# archived producer version fixture\n")
        original_identity = digest(producer_script)
        producer["runner_sha256"] = original_identity
        if "producer_runner_sha256" in producer:
            producer["producer_runner_sha256"] = original_identity
        self.refresh_retained_hash("producer-runner.py")
        # Model a completed route whose previous analyzer rejected rounded MHz.
        producer["error_stage"] = "timing_evidence"
        producer["error"] = "Incorrect timing constraint for clk"
        (self.build / "producer-qualification.json").write_text(json.dumps(producer))
        self.refresh_retained_hash("producer-qualification.json")
        manifest_path = self.build / "retained-evidence.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["hashes_captured_by"] = "recovery_analysis_before_parser_change"
        manifest["note"] = "Fixture archived evidence before parser recovery"
        manifest_path.write_text(json.dumps(manifest))
        code, report = self.analyze_existing()
        self.assertEqual(code, 0)
        self.assertTrue(report["routed"])
        self.assertFalse(report["timing_qualified"])
        self.assertTrue(report["analysis_of_existing_run"])
        self.assertEqual(report["retained_evidence_hashes_captured_by"], "recovery_analysis_before_parser_change")
        self.assertEqual(report["retained_evidence_note"], "Fixture archived evidence before parser recovery")
        self.assertEqual(report["runner_sha256"], original_identity)
        self.assertEqual(report["producer_runner_sha256"], original_identity)
        self.assertEqual(report["analysis_runner_sha256"], digest(Path(pnr.__file__)))
        self.assertNotEqual(report["runner_sha256"], report["analysis_runner_sha256"])
        self.assertEqual(report["clocks"]["clk"]["constraint"], 100.80644989013672)
        code, report = self.analyze_existing(require_timing=True)
        self.assertEqual(code, 2)
        self.assertFalse(report["timing_qualified"])

    def test_retained_evidence_manifest_requires_every_producer_artifact(self):
        self.assertEqual(self.invoke()[0], 0)
        path = self.build / "retained-evidence.json"
        manifest = json.loads(path.read_text())
        required = {"producer-runner.py", "producer-qualification.json", "routed.json", "timing.json",
                    "pnr.log", "synth.json", "seed_rom.v", "pack-proof.json", "clocks.py"}
        self.assertTrue(required.issubset(manifest["sha256"]))
        del manifest["sha256"]["seed_rom.v"]
        path.write_text(json.dumps(manifest))
        code, report = self.analyze_existing()
        self.assertNotEqual(code, 0)
        self.assertFalse(report["routed"])
        self.assertFalse(report["timing_qualified"])

    def test_retained_artifact_tampering_is_rejected_before_any_native_call(self):
        self.assertEqual(self.invoke()[0], 0)
        for name in ("producer-runner.py", "producer-qualification.json", "routed.json", "timing.json",
                     "pnr.log", "synth.json", "seed_rom.v", "pack-proof.json", "clocks.py"):
            with self.subTest(artifact=name):
                path = self.build / name
                original = path.read_bytes()
                path.write_bytes(original + b"tampered\n")
                code, report = self.analyze_existing()
                self.assertNotEqual(code, 0)
                self.assertFalse(report["routed"])
                self.assertFalse(report["timing_qualified"])
                path.write_bytes(original)

    def test_additional_manifest_files_are_allowed_but_their_hashes_are_checked(self):
        self.assertEqual(self.invoke()[0], 0)
        notes = self.build / "notes.txt"
        notes.write_text("retained notes\n")
        self.refresh_retained_hash("notes.txt")
        self.assertEqual(self.analyze_existing()[0], 0)
        notes.write_text("changed notes\n")
        code, report = self.analyze_existing()
        self.assertNotEqual(code, 0)
        self.assertFalse(report["routed"])

    def test_producer_runner_identity_and_device_command_cannot_be_rewritten(self):
        self.assertEqual(self.invoke()[0], 0)
        path = self.build / "producer-qualification.json"
        original = json.loads(path.read_text())
        for field in ("runner_sha256", "command"):
            with self.subTest(field=field):
                producer = copy.deepcopy(original)
                if field == "runner_sha256":
                    producer[field] = "0" * 64
                else:
                    producer["command"][producer["command"].index("--device") + 1] = "GW1N-9"
                path.write_text(json.dumps(producer))
                self.refresh_retained_hash("producer-qualification.json")
                code, report = self.analyze_existing()
                self.assertNotEqual(code, 0)
                self.assertFalse(report["routed"])
                self.assertFalse(report["timing_qualified"])
        path.write_text(json.dumps(original))
        self.refresh_retained_hash("producer-qualification.json")

    def test_recovery_rejects_current_sources_that_no_longer_match_pack(self):
        self.assertEqual(self.invoke()[0], 0)
        (self.root / "rtl/uv_pipeline.v").write_text("changed after the retained run\n")
        code, report = self.analyze_existing()
        self.assertNotEqual(code, 0)
        self.assertFalse(report["routed"])
        self.assertFalse(report["timing_qualified"])

    def test_routed_probe_never_qualifies_production_board(self):
        code, report, calls = self.invoke(require_timing=True)
        self.assertEqual(code, 0)
        self.assertTrue(report["placed"] and report["routed"] and report["timing_qualified"])
        self.assertFalse(report["whole_gpu_fit_qualified"])
        self.assertFalse(report["production_pll_tmds_integrated"])
        command = calls[-1]
        self.assertIn("--timing-allow-fail", command)
        self.assertEqual(command[command.index("--freq") + 1], "100.8")
        self.assertEqual(command[command.index("--threads") + 1], "2")
        self.assertIn("--pre-place", command)
        self.assertNotIn("--pre-pack", command)
        clock_script = (self.build / "clocks.py").read_text()
        self.assertIn("core_outputs[105]", clock_script)
        self.assertIn("core.host_clk", clock_script)
        self.assertIn("25.2", clock_script)

    def test_post_route_passthrough_cells_are_distinct_from_pack_and_timing_counts(self):
        timing = good_timing()
        timing["utilization"] = {"LUT4": {"used": 1, "available": 20736}}
        code, report, _ = self.invoke(timing=timing)
        self.assertEqual(code, 0)
        self.assertEqual(report["initial_packed_resources"]["LUT4"]["used"], 1)
        self.assertEqual(report["utilization"]["LUT4"]["used"], 1)
        self.assertEqual(report["post_route_physical_resources"]["LUT4"]["used"], 3)
        self.assertEqual(report["post_route_lut_site_delta"], 2)
        self.assertTrue(report["post_route_counts_include_passthrough_luts"])
        self.assertTrue(report["post_route_resources_within_capacity"])

    def test_post_route_capacity_overrun_fails_before_timing_qualification(self):
        cells = {str(index): placed_cell("LUT4", index) for index in range(20737)}
        code, report, _ = self.invoke(physical_cells=cells)
        self.assertNotEqual(code, 0)
        self.assertEqual(report["error_stage"], "physical_capacity")
        self.assertEqual(report["post_route_physical_resources"]["LUT4"]["used"], 20737)
        self.assertFalse(report["post_route_resources_within_capacity"])
        self.assertFalse(report["timing_qualified"])

    def test_empty_routed_cell_inventory_cannot_claim_success(self):
        code, report, _ = self.invoke(physical_cells={})
        self.assertNotEqual(code, 0)
        self.assertEqual(report["error_stage"], "physical_capacity")
        self.assertFalse(report["timing_qualified"])

    def test_clock_miss_is_reported_but_only_required_timing_fails_exit_status(self):
        code, report, _ = self.invoke(timing=good_timing(core=80))
        self.assertEqual(code, 0)
        self.assertTrue(report["routed"])
        self.assertFalse(report["timing_qualified"])
        code, report, _ = self.invoke(require_timing=True, timing=good_timing(core=80))
        self.assertEqual(code, 2)
        self.assertFalse(report["timing_qualified"])

    def test_stale_global_host_target_is_rejected_even_when_route_and_fmax_pass(self):
        timing = good_timing(host=120)
        timing["fmax"]["core.host_clk"]["constraint"] = 100.8
        code, report, _ = self.invoke(timing=timing)
        self.assertNotEqual(code, 0)
        self.assertTrue(report["routed"])
        self.assertEqual(report["error_stage"], "timing_evidence")
        self.assertFalse(report["timing_qualified"])

    def test_missing_host_constraint_log_cannot_claim_success(self):
        code, report, _ = self.invoke(require_timing=True, log_host=False)
        self.assertNotEqual(code, 0)
        self.assertFalse(report["timing_qualified"])

    def test_inflight_source_mutation_invalidates_routed_current_claim(self):
        code, report, _ = self.invoke(mutate=True)
        self.assertNotEqual(code, 0)
        self.assertFalse(report["routed"])
        self.assertFalse(report["timing_qualified"])
        self.assertIn("rtl/uv_pipeline.v", report["inputs_changed_during_run"])

    def test_output_paths_cannot_overwrite_retained_pack_proof(self):
        original = (self.pack / "qualification.json").read_bytes()
        self.report.parent.mkdir(parents=True)
        self.report.write_text('{"prior_report":"preserve on invalid output path"}')
        existing_output = self.report.read_bytes()
        with self.subTest(overlap="report"):
            old_report = self.report
            self.report = self.pack / "qualification.json"
            code, _, calls = self.invoke()
            self.assertNotEqual(code, 0)
            self.assertEqual(calls, [])
            self.assertEqual((self.pack / "qualification.json").read_bytes(), original)
            self.report = old_report
        with self.subTest(overlap="build"):
            self.build = self.pack
            code, _, calls = self.invoke()
            self.assertNotEqual(code, 0)
            self.assertEqual(calls, [])
            self.assertEqual((self.pack / "qualification.json").read_bytes(), original)
            self.assertEqual(self.report.read_bytes(), existing_output)

    def test_copied_input_mutation_invalidates_current_claim(self):
        code, report, _ = self.invoke(mutate_snapshot=True)
        self.assertNotEqual(code, 0)
        self.assertFalse(report["routed"])
        self.assertFalse(report["timing_qualified"])
        self.assertIn("snapshot:synth.json", report["inputs_changed_during_run"])

    def test_zero_tool_exit_without_retained_route_flags_is_rejected(self):
        for settings in ({"place": "1", "route": "0"}, {}, {"place": "0", "route": "1"}):
            with self.subTest(settings=settings):
                code, report, _ = self.invoke(route_settings=settings)
                self.assertNotEqual(code, 0)
                self.assertFalse(report["routed"])
                self.assertFalse(report["timing_qualified"])

    def test_stale_source_fails_before_tool_and_replaces_old_success_report(self):
        self.report.parent.mkdir(parents=True)
        self.report.write_text(json.dumps({"routed": True, "timing_qualified": True}))
        (self.root / "rtl/uv_pipeline.v").write_text("stale\n")
        code, report, calls = self.invoke()
        self.assertNotEqual(code, 0)
        self.assertEqual(calls, [])
        self.assertFalse(report["routed"])
        self.assertFalse(report["timing_qualified"])


if __name__ == "__main__":
    unittest.main()
