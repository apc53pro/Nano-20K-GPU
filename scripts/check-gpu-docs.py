#!/usr/bin/env python3
"""Require the GPU's two authoritative guides alongside implementation changes.

This is a read-only commit-range check. It enforces that documentation changes
are present, not that their contents are semantically correct; review is still
required. Both guide files are checked in the selected head commit, so unstaged
files cannot accidentally make a committed change pass.
"""

import argparse
from collections import defaultdict
from pathlib import Path, PurePosixPath
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
ARCHITECTURE = "GPU_DOCUMENTATION/GPU_ARCHITECTURE.md"
CONTROL = "GPU_DOCUMENTATION/GPU_CONTROL.md"
DOCUMENTS = (ARCHITECTURE, CONTROL)

ARCHITECTURE_FILES = {
    "reference/include/nano_gpu/reference.hpp",
    "reference/src/setup.cpp", "reference/src/renderer.cpp",
    "docs/hardware/rtl-core-pack.json",
}
CONTROL_FILES = {
    "rtl/gpu/nano_gpu_core.sv", "rtl/gpu/nano_host_link.sv",
    "rtl/gpu/nano_tile_renderer.sv", "rtl/gpu/nano_texture_cache.sv",
    "reference/include/nano_gpu/reference.hpp",
    "reference/include/nano_gpu/hw/protocol.hpp",
    "reference/include/nano_gpu/hw/host_link.hpp",
    "reference/include/nano_gpu/hw/resources.hpp",
    "reference/src/hw_protocol.cpp", "reference/src/hw_host_link.cpp",
    "reference/src/hw_system.cpp", "reference/src/hw_system_copy.cpp",
}


def required_updates(changed_paths):
    """Any architecture/control change requires both authoritative guides."""
    required = defaultdict(set)
    for name in set(changed_paths):
        path = PurePosixPath(name)
        hardware = name.startswith("rtl/") and not name.startswith("rtl/tests/")
        if (name in ARCHITECTURE_FILES or
                (hardware and path.suffix in {".v", ".sv", ".vh", ".svh", ".cst", ".sdc", ".hex"}) or
                (name.startswith("reference/include/nano_gpu/hw/") and path.suffix == ".hpp") or
                (name.startswith("reference/src/hw_") and path.suffix == ".cpp")):
            required[ARCHITECTURE].add(name)
        # Known wire/state implementations plus future explicitly named host
        # interfaces. Production pin/timing constraints also affect the driver.
        host_interface = (hardware and path.suffix in {".v", ".sv", ".vh", ".svh"} and
                          ("host" in path.stem or "parallel" in path.stem))
        board_constraints = (hardware and path.suffix in {".cst", ".sdc"} and
                             not path.stem.endswith("_probe"))
        if name in CONTROL_FILES or host_interface or board_constraints:
            required[CONTROL].add(name)
    # The project's documentation rule deliberately requires both guides even
    # when the implementation edit affects only one side of the interface.
    # They must describe the same architecture/control revision together.
    triggers = set().union(*required.values()) if required else set()
    return {document: sorted(triggers) for document in DOCUMENTS} if triggers else {}


def git(repo, *arguments):
    result = subprocess.run(["git", "-C", str(repo), *arguments],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError("git " + arguments[0] + " failed: " + message)
    return result.stdout


def resolve_commit(repo, reference):
    return git(repo, "rev-parse", "--verify", "--end-of-options",
               reference + "^{commit}").decode("ascii").strip()


def changed_paths(repo, base, head):
    # Name-status preserves both sides of renames/copies. Considering the old
    # name prevents moving a hardware file outside rtl/ from bypassing the rule.
    raw = git(repo, "diff", "--name-status", "-z", "--find-renames", "--no-relative",
              base, head, "--")
    fields = raw.decode("utf-8", errors="surrogateescape").split("\0")
    if fields[-1] != "":
        raise RuntimeError("git diff returned malformed name-status data")
    fields.pop()
    paths = set()
    index = 0
    while index < len(fields):
        status = fields[index]
        count = 2 if status.startswith(("R", "C")) else 1
        if not status or index + count >= len(fields):
            raise RuntimeError("git diff returned incomplete name-status data")
        paths.update(fields[index + 1:index + count + 1])
        index += count + 1
    return paths


def document_error(repo, head, document):
    entry = git(repo, "ls-tree", "-z", head, "--", document)
    if not entry:
        return "Missing authoritative guide in head commit: " + document
    metadata = entry.split(b"\t", 1)[0].split()
    if len(metadata) != 3 or metadata[0] not in (b"100644", b"100755") or metadata[1] != b"blob":
        return "Authoritative guide must be a regular Markdown file: " + document
    content = git(repo, "cat-file", "blob", head + ":" + document)
    try:
        text = content.decode("utf-8")
    except UnicodeDecodeError:
        return "Authoritative guide must use UTF-8: " + document
    if not text.strip():
        return "Authoritative guide is empty: " + document
    return None


def document_blob(repo, commit, document):
    entry = git(repo, "ls-tree", "-z", commit, "--", document)
    if not entry:
        return None
    metadata = entry.split(b"\t", 1)[0].split()
    return metadata[2] if len(metadata) == 3 and metadata[1] == b"blob" else None


def audit(repo, base, head="HEAD"):
    repo = Path(git(repo, "rev-parse", "--show-toplevel").decode("utf-8").strip())
    base_commit = resolve_commit(repo, base)
    head_commit = resolve_commit(repo, head)
    paths = changed_paths(repo, base_commit, head_commit)
    required = required_updates(paths)
    errors = []
    for document in DOCUMENTS:
        error = document_error(repo, head_commit, document)
        if error:
            errors.append(error)
    for document, sources in sorted(required.items()):
        if document not in paths or document_blob(repo, base_commit, document) == document_blob(repo, head_commit, document):
            shown = sources[:5]
            suffix = " (+{} more)".format(len(sources) - len(shown)) if len(sources) > len(shown) else ""
            errors.append("Update the contents of {} in the same change as: {}{}".format(document, ", ".join(shown), suffix))
    return {"base": base_commit, "head": head_commit, "changed_paths": sorted(paths),
            "required_updates": required, "errors": errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="existing base commit/ref; fetch it before checking")
    parser.add_argument("--head", default="HEAD", help="head commit/ref to check (default: HEAD)")
    parser.add_argument("--repo", type=Path, default=ROOT, help="repository checkout (default: this project)")
    args = parser.parse_args()
    try:
        report = audit(args.repo, args.base, args.head)
    except (OSError, RuntimeError) as error:
        print("GPU documentation check could not run: " + str(error), file=sys.stderr)
        return 2
    if report["errors"]:
        print("GPU documentation check failed:", file=sys.stderr)
        for error in report["errors"]:
            print("  - " + error, file=sys.stderr)
        return 1
    print("GPU documentation check passed: {} changed paths; both guides are present.".format(
        len(report["changed_paths"])))
    print("Documentation contents still require review against the implementation.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
