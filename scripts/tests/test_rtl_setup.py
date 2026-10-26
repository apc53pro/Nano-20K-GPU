"""Exercise the installer boundary without installing packages or compiling RTL."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SETUP = ROOT / "scripts/setup-rtl-sim.sh"


@unittest.skipUnless(shutil.which("bash"), "Bash is required for the setup script")
class RtlSetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="nano-rtl-setup-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        self.log = self.directory / "calls.txt"
        self.environment = os.environ.copy()
        self.environment.update(
            PATH=str(self.bin) + os.pathsep + self.environment.get("PATH", ""),
            MSYSTEM="UCRT64", PYTHON="nano-test-python",
            CALL_LOG=self.log.as_posix(), PACKAGE_QUERY_RC="1", UPGRADE_QUERY_RC="1",
            UPGRADE_QUERY_TEXT="", INSTALL_RC="0", PYTHON_RC="0")
        self.write_tool("uname", "printf 'MINGW64_NT-10.0\\n'\n")
        self.write_tool("nano-test-python", "printf 'python %s\\n' \"$*\" >> \"$CALL_LOG\"\nexit \"$PYTHON_RC\"\n")
        self.write_tool("pacman", """printf 'pacman %s\\n' "$*" >> "$CALL_LOG"
case "$1" in
    -Q) exit "$PACKAGE_QUERY_RC" ;;
    -Qu)
        if [[ -n "$UPGRADE_QUERY_TEXT" ]]; then printf '%s\\n' "$UPGRADE_QUERY_TEXT"; fi
        exit "$UPGRADE_QUERY_RC" ;;
    -S)
        if [[ "$INSTALL_RC" != 0 ]]; then printf 'mock GCC runtime dependency conflict\\n' >&2; fi
        exit "$INSTALL_RC" ;;
    *) exit 99 ;;
esac
""")

    def write_tool(self, name, body):
        path = self.bin / name
        path.write_text("#!/usr/bin/env bash\n" + body, encoding="utf-8", newline="\n")
        path.chmod(0o755)

    def run_setup(self, *arguments, **environment):
        self.environment.update(environment)
        return subprocess.run([shutil.which("bash"), SETUP.as_posix(), *arguments],
                              env=self.environment, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=20)

    def calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_check_never_invokes_package_manager(self):
        result = self.run_setup("--check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.calls()), 1)
        self.assertTrue(self.calls()[0].endswith("scripts/gpu-rtl.py --check --require-sdl"))
        self.assertIn("make rtl-gpu-run", result.stdout)

    def test_reuses_complete_installation(self):
        result = self.run_setup(PACKAGE_QUERY_RC="0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Reusing installed", result.stdout)
        self.assertEqual(sum(line.startswith("pacman ") for line in self.calls()), 1)
        self.assertTrue(self.calls()[-1].startswith("python -u "))

    def test_installs_native_dependencies_when_missing(self):
        result = self.run_setup()
        self.assertEqual(result.returncode, 0, result.stderr)
        installation = [line for line in self.calls() if line.startswith("pacman -S ")]
        self.assertEqual(len(installation), 1)
        for package in ("make", "perl", "mingw-w64-ucrt-x86_64-gcc",
                        "mingw-w64-ucrt-x86_64-gcc-libs", "mingw-w64-ucrt-x86_64-cc-libs",
                        "mingw-w64-ucrt-x86_64-python", "mingw-w64-ucrt-x86_64-pkgconf",
                        "mingw-w64-ucrt-x86_64-SDL2", "mingw-w64-ucrt-x86_64-verilator"):
            self.assertIn(package, installation[0].split())
        self.assertTrue(self.calls()[-1].startswith("python -u "))

    def test_pending_upgrades_do_not_start_partial_install(self):
        result = self.run_setup(UPGRADE_QUERY_RC="0", UPGRADE_QUERY_TEXT="gcc 15 -> 16")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pacman -Syu", result.stderr)
        self.assertIn("reopen MSYS2 UCRT64", result.stderr)
        self.assertFalse(any(line.startswith("pacman -S ") or line.startswith("python ")
                             for line in self.calls()))

    def test_installation_failure_preserves_status_and_upgrade_diagnostic(self):
        result = self.run_setup(INSTALL_RC="42")
        self.assertEqual(result.returncode, 42)
        self.assertIn("mock GCC runtime dependency conflict", result.stderr)
        self.assertIn("pacman -Syu", result.stderr)
        self.assertIn("Do not remove gcc-libs", result.stderr)
        self.assertIn("pacman -Syyu mingw-w64-ucrt-x86_64-gcc-libs mingw-w64-ucrt-x86_64-cc-libs",
                      result.stderr)
        self.assertFalse(any(line.startswith("python ") for line in self.calls()))

    def test_package_database_error_is_not_treated_as_no_upgrades(self):
        result = self.run_setup(UPGRADE_QUERY_RC="3", UPGRADE_QUERY_TEXT="database unavailable")
        self.assertEqual(result.returncode, 3)
        self.assertIn("database unavailable", result.stderr)
        self.assertFalse(any(line.startswith("pacman -S ") for line in self.calls()))

    def test_missing_python_check_has_actionable_error(self):
        result = self.run_setup("--check", PYTHON="nano-deliberately-missing-python")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Python 3 was not found", result.stderr)
        self.assertIn("rerun setup without --check", result.stderr)

    def test_failed_preflight_is_not_reported_ready(self):
        result = self.run_setup("--check", PYTHON_RC="17")
        self.assertEqual(result.returncode, 17)
        self.assertNotIn("Simulator prerequisites are ready", result.stdout)

    def test_other_windows_environments_are_rejected(self):
        result = self.run_setup(MSYSTEM="MINGW64")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("MSYS2 UCRT64", result.stderr)
        self.assertEqual(self.calls(), [])

    def test_git_bash_without_pacman_is_rejected(self):
        (self.bin / "pacman").unlink()
        probe = subprocess.run([shutil.which("bash"), "-c",
                                "command -v pacman >/dev/null 2>&1 || test -x /usr/bin/pacman.exe"],
                               env=self.environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if probe.returncode == 0:
            self.skipTest("a real MSYS2 pacman remains available on this host")
        result = self.run_setup("--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not a complete MSYS2 installation", result.stderr)
        self.assertIn("Git Bash is a separate installation", result.stderr)
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    unittest.main()
