"""Portable regression coverage for native MSYS2 RTL build diagnostics."""

from contextlib import ExitStack, redirect_stdout
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gpu_rtl_builder", ROOT / "scripts/gpu-rtl.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class VerilatorLauncherTests(unittest.TestCase):
    def setUp(self):
        self.contexts = ExitStack()
        self.addCleanup(self.contexts.close)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.suite = self.root / "suite"
        (self.suite / "bin").mkdir(parents=True)
        self.tools = self.root / "ucrt64" / "bin"
        self.tools.mkdir(parents=True)
        self.contexts.enter_context(patch.object(builder.HDL, "SUITE", self.suite))
        self.contexts.enter_context(patch.object(builder.HDL, "environment", return_value={"PATH": str(self.tools)}))

    def test_native_executable_does_not_require_msys_perl(self):
        native = self.tools / "verilator.exe"
        native.touch()
        with patch.object(builder.shutil, "which", side_effect=AssertionError("Perl must not be queried")):
            self.assertEqual(builder.verilator_command(windows=True), [native.as_posix()])

    def test_msys_launcher_runs_through_the_official_msys_perl(self):
        launcher = self.tools / "verilator"
        launcher.write_text("#!/usr/bin/perl\n")
        # verilator_bin.exe alone is not a replacement for the launcher.
        (self.tools / "verilator_bin.exe").touch()
        perl = self.root / "usr" / "bin" / "perl.exe"
        perl.parent.mkdir(parents=True)
        perl.touch()
        converted = []

        def cygpath(command, **_kwargs):
            converted.append(command[1:])
            if command[1:] == ["-w", "/usr/bin/perl.exe"]:
                return str(perl) + "\n"
            if command[1:] == ["-u", launcher.as_posix()]:
                return "/ucrt64/bin/verilator\n"
            self.fail("Unexpected path conversion: " + repr(command))

        with patch.object(builder.shutil, "which", return_value="cygpath.exe"), \
                patch.object(builder.subprocess, "check_output", side_effect=cygpath):
            self.assertEqual(builder.verilator_command(windows=True),
                             [perl.as_posix(), "/ucrt64/bin/verilator"])
        self.assertEqual(len(converted), 2)

    def test_missing_cygpath_gives_setup_instructions(self):
        (self.tools / "verilator").touch()
        with patch.object(builder.shutil, "which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "MSYS2.*setup-rtl-sim"):
                builder.verilator_command(windows=True)

    def test_missing_perl_is_reported_before_executing_a_perl_script(self):
        (self.tools / "verilator").touch()
        with patch.object(builder.shutil, "which", return_value="cygpath.exe"), \
                patch.object(builder.subprocess, "check_output", return_value=str(self.root / "missing-perl.exe")):
            with self.assertRaisesRegex(RuntimeError, "Perl is missing.*setup-rtl-sim"):
                builder.verilator_command(windows=True)

    def test_missing_verilator_gives_the_windows_setup_command(self):
        with self.assertRaisesRegex(RuntimeError, "Verilator is missing.*setup-rtl-sim"):
            builder.verilator_command(windows=True)

    def test_linux_keeps_the_existing_hdl_tool_selection(self):
        with patch.object(builder.HDL, "tool", return_value=["/sdk/verilator"]) as tool:
            self.assertEqual(builder.verilator_command(windows=False), ["/sdk/verilator"])
        tool.assert_called_once_with("verilator")


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.contexts = ExitStack()
        self.addCleanup(self.contexts.close)
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.contexts.enter_context(patch.object(builder, "ROOT", self.root))
        self.contexts.enter_context(patch.object(builder.HDL, "environment", return_value={"PATH": "/test/tools"}))
        self.contexts.enter_context(patch.object(builder.shutil, "which", return_value="/test/tools/make"))
        self.contexts.enter_context(patch.object(builder, "verilator_command", return_value=["verilator-test"]))
        self.tool_output = self.contexts.enter_context(patch.object(builder, "run", return_value="test tool version\n"))
        self.contexts.enter_context(redirect_stdout(io.StringIO()))

    def test_missing_make_reports_the_setup_command(self):
        with patch.object(builder.shutil, "which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "make is missing.*setup-rtl-sim"):
                builder.preflight(self.root)

    def test_unusable_compiler_gives_the_same_setup_path(self):
        self.tool_output.side_effect = FileNotFoundError("g++ not found")
        with self.assertRaisesRegex(RuntimeError, "Cannot run the RTL toolchain.*setup-rtl-sim"):
            builder.preflight(self.root)

    def test_required_sdl_cannot_silently_produce_a_headless_binary(self):
        with patch.object(builder.subprocess, "check_output", side_effect=FileNotFoundError("pkg-config not found")):
            with self.assertRaisesRegex(RuntimeError, "SDL2 development files are missing.*setup-rtl-sim"):
                builder.preflight(self.root, require_sdl=True)

    def test_optional_sdl_failure_still_supports_an_explicit_headless_workflow(self):
        with patch.object(builder.subprocess, "check_output", side_effect=subprocess.CalledProcessError(1, "pkg-config")):
            command, flags, links, sdl = builder.preflight(self.root)
        self.assertEqual(command, ["verilator-test"])
        self.assertFalse(sdl)
        self.assertEqual(links, [])
        self.assertNotIn("-DNANO_GPU_RTL_SDL=1", flags)

    def test_headless_check_does_not_require_pkg_config(self):
        with patch.object(builder.subprocess, "check_output", side_effect=AssertionError("No SDL query expected")):
            _command, _flags, links, sdl = builder.preflight(self.root, headless_only=True)
        self.assertFalse(sdl)
        self.assertEqual(links, [])

    def test_available_sdl_flags_are_used_for_the_real_build(self):
        def pkg_config(command, **_kwargs):
            if command[1] == "--cflags": return "-I/test/SDL2 -D_REENTRANT\n"
            if command[1] == "--libs": return "-L/test/lib -lSDL2\n"
            if command[1] == "--variable=libdir": return "/test/lib\n"
            self.fail("Unexpected pkg-config command: " + repr(command))

        with patch.object(builder.subprocess, "check_output", side_effect=pkg_config):
            _command, flags, links, sdl = builder.preflight(self.root, require_sdl=True)
        self.assertTrue(sdl)
        self.assertIn("-DNANO_GPU_RTL_SDL=1", flags)
        self.assertIn("-I/test/SDL2", flags)
        self.assertIn("-lSDL2", links)


class BuildDiagnosticsTests(unittest.TestCase):
    def test_failure_retains_the_actual_compiler_diagnostic_in_both_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            for verbose in (False, True):
                with self.subTest(verbose=verbose), patch.object(builder, "VERBOSE", verbose):
                    log = Path(directory) / ("verbose.log" if verbose else "normal.log")
                    captured = io.StringIO()
                    command = [sys.executable, "-c", "import sys; print('fatal: missing header', file=sys.stderr); sys.exit(7)"]
                    with redirect_stdout(captured):
                        with self.assertRaisesRegex(RuntimeError, "fatal: missing header") as failure:
                            builder.run(command, log)
                    self.assertIn(str(log), str(failure.exception))
                    self.assertIn("fatal: missing header", log.read_text())
                    self.assertIn(str(log), captured.getvalue())

    def test_quiet_compiler_periodically_reports_that_the_build_is_running(self):
        with tempfile.TemporaryDirectory() as directory:
            command = ["quiet-compiler"]
            process = Mock()
            process.wait.side_effect = [subprocess.TimeoutExpired(command, 5), 0]
            captured = io.StringIO()
            with patch.object(builder, "VERBOSE", False), \
                    patch.object(builder.subprocess, "Popen", return_value=process), \
                    patch.object(builder.time, "monotonic", side_effect=[100, 105]), \
                    redirect_stdout(captured):
                builder.run(command, Path(directory) / "compiler.log")
            self.assertIn("Still building (5s)", captured.getvalue())


if __name__ == "__main__":
    unittest.main()
