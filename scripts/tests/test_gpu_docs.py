"""Commit-range enforcement for the two authoritative GPU guides."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/check-gpu-docs.py"
SPEC = importlib.util.spec_from_file_location("gpu_docs_guard", SCRIPT)
guard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guard)


class DocumentationRulesTests(unittest.TestCase):
    def test_pipeline_change_requires_both_guides(self):
        result = guard.required_updates({"rtl/uv_pipeline.v"})
        self.assertEqual(set(result), set(guard.DOCUMENTS))

    def test_command_core_requires_both_guides(self):
        result = guard.required_updates({"rtl/gpu/nano_gpu_core.sv"})
        self.assertEqual(set(result), set(guard.DOCUMENTS))

    def test_wire_abi_and_public_render_state_require_both_guides(self):
        for path in ("reference/src/hw_protocol.cpp",
                     "reference/include/nano_gpu/hw/resources.hpp",
                     "reference/include/nano_gpu/reference.hpp"):
            with self.subTest(path=path):
                self.assertEqual(set(guard.required_updates({path})), set(guard.DOCUMENTS))

    def test_future_hardware_and_host_interfaces_are_not_silently_ignored(self):
        result = guard.required_updates({"rtl/board/new_parallel_pad.sv"})
        self.assertEqual(set(result), set(guard.DOCUMENTS))

    def test_rtl_include_files_cannot_bypass_the_update_rule(self):
        for path in ("rtl/gpu/registers.svh", "rtl/constants.vh"):
            with self.subTest(path=path):
                self.assertEqual(set(guard.required_updates({path})), set(guard.DOCUMENTS))

    def test_constraint_changes_require_both_guides(self):
        for path in ("rtl/board/gpu.cst", "rtl/gpu_probe.cst"):
            self.assertEqual(set(guard.required_updates({path})), set(guard.DOCUMENTS))

    def test_test_assets_and_build_only_edits_do_not_force_gpu_guide_edits(self):
        self.assertEqual(guard.required_updates({"rtl/tests/native_video_tb.sv",
                                                "scripts/gpu-rtl.py", "README.md"}), {})


class CommittedDocumentationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.repo = Path(self.directory.name)
        self.git("init", "--quiet")
        self.write(guard.ARCHITECTURE, "# Architecture\nInitial architecture.\n")
        self.write(guard.CONTROL, "# Control\nInitial control protocol.\n")
        self.write("rtl/gpu/nano_gpu_core.sv", "module nano_gpu_core; endmodule\n")
        self.write("rtl/uv_pipeline.v", "module uv_pipeline; endmodule\n")
        self.base = self.commit("initial")

    def git(self, *arguments):
        return subprocess.check_output(["git", "-C", str(self.repo), *arguments], stderr=subprocess.PIPE)

    def write(self, name, text):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def commit(self, message):
        self.git("add", "-A")
        self.git("-c", "user.name=Documentation Test", "-c", "user.email=docs-test@example.invalid",
                 "commit", "--quiet", "-m", message)
        return self.git("rev-parse", "HEAD").decode().strip()

    def test_unrelated_commit_with_existing_guides_passes(self):
        self.write("README.md", "Project setup changed.\n")
        self.commit("readme")
        self.assertEqual(guard.audit(self.repo, self.base)["errors"], [])

    def test_code_without_guides_fails_and_names_both_required_files(self):
        self.write("rtl/gpu/nano_gpu_core.sv", "module nano_gpu_core; wire changed; endmodule\n")
        self.commit("change command core")
        report = guard.audit(self.repo, self.base)
        self.assertEqual(len(report["errors"]), 2)
        for document in guard.DOCUMENTS:
            self.assertTrue(any(document in error for error in report["errors"]))

    def test_matching_guide_edits_in_the_same_range_pass(self):
        self.write("rtl/gpu/nano_gpu_core.sv", "module nano_gpu_core; wire changed; endmodule\n")
        self.commit("implementation")
        self.write(guard.ARCHITECTURE, "# Architecture\nChanged command execution.\n")
        self.write(guard.CONTROL, "# Control\nChanged command semantics.\n")
        self.commit("document implementation")
        self.assertEqual(guard.audit(self.repo, self.base)["errors"], [])

    def test_uncommitted_documentation_cannot_make_a_committed_change_pass(self):
        self.write("rtl/uv_pipeline.v", "module uv_pipeline; wire changed; endmodule\n")
        self.commit("arithmetic change")
        self.write(guard.ARCHITECTURE, "# Architecture\nUncommitted update.\n")
        report = guard.audit(self.repo, self.base)
        self.assertTrue(any(guard.ARCHITECTURE in error for error in report["errors"]))

    def test_only_one_guide_update_does_not_satisfy_an_architecture_change(self):
        self.write("rtl/uv_pipeline.v", "module uv_pipeline; wire changed; endmodule\n")
        self.write(guard.ARCHITECTURE, "# Architecture\nUpdated arithmetic.\n")
        self.commit("update arithmetic and only one guide")
        errors = guard.audit(self.repo, self.base)["errors"]
        self.assertEqual(len(errors), 1)
        self.assertIn(guard.CONTROL, errors[0])

    def test_mode_only_guide_changes_do_not_count_as_documentation_updates(self):
        self.write("rtl/uv_pipeline.v", "module uv_pipeline; wire changed; endmodule\n")
        self.git("add", "-A")
        self.git("update-index", "--chmod=+x", *guard.DOCUMENTS)
        self.git("-c", "user.name=Documentation Test", "-c", "user.email=docs-test@example.invalid",
                 "commit", "--quiet", "-m", "change hardware and guide modes only")
        report = guard.audit(self.repo, self.base)
        self.assertTrue(set(guard.DOCUMENTS).issubset(report["changed_paths"]))
        self.assertEqual(len(report["errors"]), 2)
        self.assertTrue(all("contents" in error for error in report["errors"]))

    def test_renaming_hardware_out_of_watched_directory_still_requires_guide(self):
        self.git("mv", "rtl/uv_pipeline.v", "old_arithmetic.txt")
        self.commit("move hardware")
        report = guard.audit(self.repo, self.base)
        self.assertIn("rtl/uv_pipeline.v", report["changed_paths"])
        self.assertIn("old_arithmetic.txt", report["changed_paths"])
        self.assertTrue(any(guard.ARCHITECTURE in error for error in report["errors"]))

    def test_renaming_into_hardware_directory_is_detected(self):
        self.write("candidate.txt", "module future; endmodule\n")
        base = self.commit("candidate")
        self.git("mv", "candidate.txt", "rtl/future.sv")
        self.commit("add hardware")
        self.assertIn(guard.ARCHITECTURE, guard.audit(self.repo, base)["required_updates"])

    def test_deleted_hardware_is_not_ignored(self):
        (self.repo / "rtl/gpu/nano_gpu_core.sv").unlink()
        self.commit("remove command core")
        self.assertEqual(set(guard.audit(self.repo, self.base)["required_updates"]), set(guard.DOCUMENTS))

    def test_missing_or_empty_committed_guide_is_rejected_even_for_docs_only_change(self):
        (self.repo / guard.CONTROL).unlink()
        self.write(guard.ARCHITECTURE, "  \n")
        self.commit("break documentation")
        errors = guard.audit(self.repo, self.base)["errors"]
        self.assertEqual(len(errors), 2)
        self.assertTrue(any("Missing" in error and guard.CONTROL in error for error in errors))
        self.assertTrue(any("empty" in error and guard.ARCHITECTURE in error for error in errors))

    def test_invalid_reference_fails_closed(self):
        with self.assertRaises(RuntimeError):
            guard.audit(self.repo, "reference-that-does-not-exist")

    def test_first_documentation_commit_can_be_checked_against_earlier_code(self):
        # The guide need not have existed in the base; initial rollout is valid.
        self.git("rm", "--quiet", *guard.DOCUMENTS)
        base = self.commit("old tree without guides")
        self.write(guard.ARCHITECTURE, "# Architecture\nNew authoritative guide.\n")
        self.write(guard.CONTROL, "# Control\nNew authoritative guide.\n")
        self.commit("introduce guides")
        self.assertEqual(guard.audit(self.repo, base)["errors"], [])

    def test_cli_success_and_invalid_revision_exit_codes(self):
        passed = subprocess.run([sys.executable, str(SCRIPT), "--repo", str(self.repo), "--base", self.base],
                                capture_output=True, text=True)
        self.assertEqual(passed.returncode, 0, passed.stderr)
        self.assertIn("still require review", passed.stdout)
        invalid = subprocess.run([sys.executable, str(SCRIPT), "--repo", str(self.repo), "--base", "missing"],
                                 capture_output=True, text=True)
        self.assertEqual(invalid.returncode, 2)
        self.assertIn("could not run", invalid.stderr)


if __name__ == "__main__":
    unittest.main()
