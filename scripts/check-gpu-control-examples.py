#!/usr/bin/env python3
"""Compile the control guide's actual C99 example and exercise its bus traffic."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
CORE_OBJECTS = (
    "setup", "renderer", "hw_mult18x18", "hw_bsram", "hw_tile_pipeline",
    "hw_perspective", "hw_texture", "hw_walker2d", "hw_blit", "hw_sdram",
    "hw_host_link", "hw_scanout", "hw_protocol", "hw_system_copy", "hw_system",
    "hw_reciprocal", "hw_fragment_pipeline",
)


def command(variable: str, default: str) -> list[str]:
    return shlex.split(os.environ.get(variable, default))


def run(argv: list[str]) -> None:
    print("+ " + shlex.join(argv), flush=True)
    subprocess.run(argv, cwd=ROOT, check=True)


def extract_example(guide: Path) -> str:
    text = guide.read_text(encoding="utf-8")
    start, end = "<!-- gpu-control-c99:start -->", "<!-- gpu-control-c99:end -->"
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError("control guide must contain exactly one pair of C99 example markers")
    section = text.split(start, 1)[1].split(end, 1)[0]
    match = re.fullmatch(r"\s*```c(?:99)?\s*\n(.*?)\n```\s*", section, re.DOTALL)
    if not match:
        raise ValueError("C99 example markers must enclose exactly one fenced c/c99 block")
    return match.group(1) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/gpu-docs"))
    parser.add_argument("--core-build-dir", type=Path, default=Path("build"))
    parser.add_argument("--skip-core-build", action="store_true")
    args = parser.parse_args()
    build = ROOT / args.build_dir
    core = ROOT / args.core_build_dir
    build.mkdir(parents=True, exist_ok=True)
    extracted = build / "gpu_control_example.inc"
    extracted.write_text(extract_example(ROOT / "GPU_DOCUMENTATION/GPU_CONTROL.md"), encoding="utf-8")

    objects = [core / (name + ".o") for name in CORE_OBJECTS]
    if not args.skip_core_build:
        targets = [os.path.relpath(path, ROOT).replace(os.sep, "/") for path in objects]
        run(command("MAKE", "make") + ["-j2", "BUILD_DIR=" + args.core_build_dir.as_posix()] + targets)
    cc, cxx = command("CC", "cc"), command("CXX", "c++")
    cppflags = command("CPPFLAGS", "")
    cflags = command("CFLAGS", "-O2 -g")
    cxxflags = command("CXXFLAGS", "-O2 -g")
    c_object, cpp_object = build / "gpu_control_docs_harness.o", build / "gpu_control_docs_tests.o"
    run(cc + cppflags + cflags + ["-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-I" + str(build), "-c", str(ROOT / "reference/tests/gpu_control_docs_harness.c"), "-o", str(c_object)])
    run(cxx + cppflags + cxxflags + ["-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I" + str(ROOT / "reference/include"), "-c",
        str(ROOT / "reference/tests/gpu_control_docs_tests.cpp"), "-o", str(cpp_object)])
    suffix = ".exe" if os.name == "nt" or os.environ.get("MSYSTEM") in {"UCRT64", "MINGW64", "CLANG64"} else ""
    executable = build / ("gpu-control-docs-tests" + suffix)
    run(cxx + cxxflags + [str(c_object), str(cpp_object)] + [str(path) for path in objects]
        + command("LDFLAGS", "") + ["-o", str(executable)])
    run([str(executable)])
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("GPU control example check failed: " + str(error), file=sys.stderr)
        sys.exit(1)
