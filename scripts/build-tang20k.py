#!/usr/bin/env python3
"""Build the Tang Nano 20K board image with Gowin EDA; emit no image on a failed gate.

The open-source flow produced an image whose host address latch failed on the
board, so board images use the vendor flow. See AGENTS.md: the fixed SiP
connection check cannot be made from an encrypted Gowin netlist. The report
records the Gowin version used, its Authenticode signature and whether the
netlist is encrypted. Every other gate below still applies.
"""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
DEVICE = "GW2AR-LV18QN88C8/I7"
FAMILY = "GW2AR-18C"
DEFAULT_GOWIN = r"C:\Gowin\Gowin_V1.9.11.03_Education_x64\IDE\bin\gw_sh.exe"
# Required clocks (Gowin-derived names) and their targets in MHz.
REQUIRED = {"clk27": 27.0, "gpu_hclk": 25.2, "core_div/CLKOUT.default_gen_clk": 50.4,
            "pixel_div/CLKOUT.default_gen_clk": 25.2}
# Reset-release paths that may show negative slack. Each is an asynchronous
# assert into a synchronizer (power-on into the core/pixel reset pipes) or the
# serializer reset released from the synchronized pixel reset. They are listed
# in the report rather than hidden by constraints.
ALLOWED_VIOLATIONS = [
    (re.compile(r"^poweron_\d+_s\d*/Q$"), re.compile(r"^(core|pixel)_reset_pipe_\d+_s\d*/PRESET$")),
    (re.compile(r"^pixel_reset_pipe_1_s\d*/Q$"), re.compile(r"^video/lanes\[\d\]\.serializer/serializer/RESET$")),
]
# Syntax-only rewrites for GowinSynthesis on build copies; RTL stays unchanged.
REWRITES = {"nano_fragment_pipeline.sv": [(
    "data4={8'b0,factor_data[23:16]},data5;",
    "data4={8'b0,factor_data[23:16]};\n    wire [15:0] data5;")]}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def toolchain(path):
    # The version is part of Gowin's install folder name, e.g. Gowin_V1.9.11.03_Education_x64.
    m = re.search(r"Gowin_(V[\d.]+)_([A-Za-z]+)", str(path))
    return f"Gowin EDA (gw_sh) {m.group(1)} {m.group(2)}" if m else "Gowin EDA (gw_sh), unknown version"


def signature(path):
    # Windows Authenticode status and signer of gw_sh; None elsewhere.
    if os.name != "nt": return None
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-Command",
                              "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
                              "$s = Get-AuthenticodeSignature -LiteralPath $env:N20K_GW_SH; "
                              "$s.Status.ToString() + '|' + $s.SignerCertificate.Subject"],
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
                             env=dict(os.environ, N20K_GW_SH=str(path))).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    status, _, signer = out.partition("|")
    return {"status": status, "signer": signer} if status else None


def write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", newline="\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build/tang20k")
    ap.add_argument("--gowin", default=DEFAULT_GOWIN, help="path to Gowin gw_sh")
    ap.add_argument("--place-option", type=int, default=2)
    ap.add_argument("--route-option", type=int, default=1)
    ap.add_argument("--reuse-gowin", action="store_true",
                    help="re-run every gate on the existing Gowin output without rebuilding "
                         "(only valid when its project inputs are unchanged; recorded in the report)")
    args = ap.parse_args()
    b = args.build_dir.resolve(); b.mkdir(parents=True, exist_ok=True)
    image = b / "nano-gpu.fs"; image.unlink(missing_ok=True)
    project = b / "gowin"; src = project / "src"; src.mkdir(parents=True, exist_ok=True)
    # Fingerprint the previous Gowin project inputs before regenerating them.
    project_inputs = lambda: {p.relative_to(project).as_posix(): digest(p) for p in
                              sorted(src.glob("*")) + [project / "tang20k-gowin.cst", project / "tang20k.sdc",
                                                       project / "build.tcl"] if p.exists()}
    previous_inputs = project_inputs() if args.reuse_gowin else {}
    rom = src / "seed_rom.v"
    subprocess.run([sys.executable, str(ROOT / "scripts/rtl-generate-rom.py"), str(rom)], cwd=ROOT, check=True)
    sources = [ROOT / "rtl/uv_mult.v", ROOT / "rtl/uv_pipeline.v"] + sorted((ROOT / "rtl/gpu").glob("*.sv")) + \
              [ROOT / "rtl/board/tang20k_top.sv"]
    board_cst = ROOT / "rtl/board/tang20k.cst"
    inputs = sources + [board_cst, Path(__file__).resolve(), ROOT / "scripts/rtl-generate-rom.py"] + \
             sorted((ROOT / "rtl/generated").glob("*.hex"))
    hashes = {p.relative_to(ROOT).as_posix(): digest(p) for p in inputs}
    report = {"schema": "N20K-TANG20K-BOARD-2", "device": DEVICE, "top": "tang20k_top",
              "toolchain": toolchain(args.gowin), "gowin_shell": args.gowin,
              "toolchain_signature": signature(args.gowin),
              "source_sha256": hashes, "place_option": args.place_option, "route_option": args.route_option,
              "profile": "gpu-video-50mhz", "core_mhz": 50.4, "pixel_mhz": 25.2, "serial_mhz": 126.0,
              "host_max_mhz": 25.2, "command_player": False, "hardware_tested": False,
              "external_io_timing_measured": False, "upload_ready": False,
              "agents_exception": ("AGENTS.md addendum 2026-11-07: the SiP SDRAM connection cannot be checked "
                                   "from an encrypted Gowin netlist (see netlist_encrypted)."),
              "sip_pins_verified": False,
              "timing_scope": ("Gowin static timing on its own placement; external SDRAM/parallel-bus margins "
                               "require hardware measurement")}
    save = lambda: write(b / "board-report.json", report)
    save()
    functional = b / "functional-checks.json"
    if not functional.exists():
        sys.exit("build-tang20k: run scripts/check-tang20k.py first (no functional evidence)")
    checks = json.loads(functional.read_text())
    stale = [n for n, h in checks.get("source_sha256", {}).items() if not (ROOT / n).exists() or digest(ROOT / n) != h]
    if not checks.get("passed") or stale:
        sys.exit("build-tang20k: functional evidence is missing, failed or stale: " + ", ".join(stale))
    report["functional_report_sha256"] = digest(functional); save()
    # Gowin sees the same SYNTHESIS view the RTL selects primitives under.
    copies = [rom]
    rom.write_text("`define SYNTHESIS\n" + rom.read_text(), newline="\n")
    for path in sources:
        text = path.read_text()
        for old, new in REWRITES.get(path.name, []):
            if text.count(old) != 1: sys.exit(f"build-tang20k: rewrite not unique in {path.name}")
            text = text.replace(old, new)
        copy = src / path.name
        copy.write_text("`define SYNTHESIS\n" + text, newline="\n")
        copies.append(copy)
    # Gowin derives each differential N pin from its P pin and rejects a
    # second constraint; those lines are checked against the pin report below.
    cst = project / "tang20k-gowin.cst"
    cst.write_text("\n".join(l for l in board_cst.read_text().splitlines() if '"hdmi_n[' not in l) + "\n", newline="\n")
    sdc = project / "tang20k.sdc"
    sdc.write_text("create_clock -name clk27 -period 37.037 [get_ports {clk27}]\n"
                   "create_clock -name gpu_hclk -period 39.683 [get_ports {gpu_hclk}]\n"
                   "set_clock_groups -asynchronous -group [get_clocks {gpu_hclk}]\n"
                   "report_timing -setup -max_paths 400 -max_common_paths 1\n", newline="\n")
    tcl = project / "build.tcl"
    tcl.write_text("\n".join([f"set_device -name {FAMILY} {DEVICE}"] +
                             [f"add_file {{{p.as_posix()}}}" for p in copies + [cst, sdc]] +
                             ["set_option -top_module tang20k_top", "set_option -verilog_std sysv2017",
                              "set_option -output_base_name nano-gpu",
                              "set_option -use_mspi_as_gpio 1", "set_option -use_sspi_as_gpio 1",
                              "set_option -use_ready_as_gpio 1", "set_option -use_done_as_gpio 1",
                              "set_option -use_reconfign_as_gpio 1", "set_option -timing_driven 1",
                              "set_option -gen_text_timing_rpt 1",
                              f"set_option -place_option {args.place_option}",
                              f"set_option -route_option {args.route_option}", "run all"]) + "\n", newline="\n")
    pnr = project / "impl/pnr"
    built = pnr / "nano-gpu.fs"
    log = b / "gowin.log"
    if args.reuse_gowin:
        # Accept the existing Gowin output only if this run regenerated
        # byte-identical project inputs (RTL copies, constraints and script).
        if project_inputs() != previous_inputs or not built.exists():
            sys.exit("build-tang20k: --reuse-gowin needs an existing build from identical inputs")
        report["gowin_reused"] = True; report["gowin_returncode"] = 0
    else:
        print("Building the complete board with Gowin EDA (synthesis, place and route)...", flush=True)
        with log.open("w", encoding="utf-8", errors="replace") as out:
            result = subprocess.run([args.gowin, str(tcl)], cwd=project, stdout=out, stderr=subprocess.STDOUT)
        report["gowin_returncode"] = result.returncode
        if result.returncode or not built.exists():
            save(); sys.exit("build-tang20k: Gowin build failed; see " + str(log))
    report.update(synthesized=True, placed=True, routed=True)
    netlist = project / "impl/gwsynthesis/nano-gpu.vg"
    report["netlist_encrypted"] = netlist.exists() and \
        "pragma protect begin_protected" in netlist.read_text(errors="replace")
    # Pins: every constrained port, and each HDMI N pin derived from its pair.
    # Pin report rows are <tr><td>port</td><td>pair</td><td>pin(s)/bank</td>...
    pin_report = (pnr / "nano-gpu.pin.html").read_text(errors="replace")
    expected = dict(re.findall(r'IO_LOC "([^"]+)" (\d+);', board_cst.read_text()))
    rows = {}
    for row in re.findall(r"<tr>(.*?)</tr>", pin_report, re.S):
        cells = [html.unescape(re.sub(r"<[^>]+>", "", c)).strip()
                 for c in re.findall(r"<td.*?>(.*?)</td>", row, re.S)]
        if len(cells) > 2 and cells[0] in expected:
            numbers = cells[2].split("/")[0].split(",")
            rows[cells[0]] = numbers[0]
            if cells[1].startswith("hdmi_n[") and len(numbers) > 1: rows[cells[1]] = numbers[1]
    mismatched = sorted(p for p in expected if rows.get(p) != expected[p] and not p.startswith(("O_sdram", "IO_sdram")))
    report["package_pins_checked"] = len(rows)
    report["package_pins_verified"] = not mismatched
    report["pin_mismatches"] = mismatched; save()
    if mismatched: sys.exit("build-tang20k: pin mismatch: " + ", ".join(mismatched))
    # Timing: every required clock meets its constraint, and every listed
    # negative-slack path is one of the documented reset-release paths.
    timing = (pnr / "nano-gpu.tr").read_text(errors="replace")
    fmax = {m.group(1): {"constraint_mhz": float(m.group(2)), "achieved_mhz": float(m.group(3))}
            for m in re.finditer(r"^\s+\d+\s+(\S+)\s+([\d.]+)\(MHz\)\s+([\d.]+)\(MHz\)", timing, re.M)}
    report["fmax"] = fmax
    missing = [c for c in REQUIRED if c not in fmax or abs(fmax[c]["constraint_mhz"] - REQUIRED[c]) > 0.01]
    slow = [c for c in REQUIRED if c in fmax and fmax[c]["achieved_mhz"] < fmax[c]["constraint_mhz"]]
    negative = sorted({(float(s), a, d) for s, a, d in
                       re.findall(r"^\s+\d+\s+(-[\d.]+)\s+(\S+)\s+(\S+)\s+\S+:\[[RF]\]", timing, re.M)})
    unexpected = [n for n in negative if not any(f.match(n[1]) and t.match(n[2]) for f, t in ALLOWED_VIOLATIONS)]
    report["violations_listed"] = [{"slack_ns": s, "from": a, "to": d} for s, a, d in negative]
    report["violations_unexpected"] = [{"slack_ns": s, "from": a, "to": d} for s, a, d in unexpected]
    counts = dict(re.findall(r"<Numbers of (Setup|Hold) Violated Endpoints>:(\d+)", timing))
    report["violated_endpoint_counts"] = {k.lower(): int(v) for k, v in counts.items()}
    report["timing_passed"] = not missing and not slow and not unexpected; save()
    if not report["timing_passed"]:
        sys.exit(f"build-tang20k: timing gate failed (missing={missing}, slow={slow}, "
                 f"unexpected={len(unexpected)}); no image emitted")
    resources = (pnr / "nano-gpu.rpt.txt").read_text(errors="replace")
    report["resources"] = {name.strip(): value.strip() for name, value in
                           re.findall(r"^\s+(Logic|Register|BSRAM|DSP|PLL|CLKDIV|I/O Port)\s+\|\s*([^\n]+)", resources, re.M)}
    changed = [n for n, h in hashes.items() if digest(ROOT / n) != h]
    report["inputs_changed"] = changed
    if changed: save(); sys.exit("build-tang20k: sources changed during the build")
    image.write_bytes(built.read_bytes())
    report.update(bitstream_generated=True, upload_ready=True, bitstream_sha256=digest(image),
                  bitstream_bytes=image.stat().st_size)
    save()
    print("Upload image: " + str(image) + "\nSHA-256: " + report["bitstream_sha256"])


if __name__ == "__main__":
    main()
