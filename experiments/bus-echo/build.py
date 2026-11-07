#!/usr/bin/env python3
"""Build the diagnostic Tang Nano 20K host-bus echo image (not the GPU image)."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("bus_echo_hdl", ROOT / "scripts/rtl-check.py")
HDL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HDL)
TOP = "tang20k_bus_echo"
DEVICE = "GW2AR-LV18QN88C8/I7"
PORTS = ["gpu_hclk", "gpu_reset_n", "gpu_abort", "gpu_ale_n", "gpu_wr_n", "gpu_rd_n",
         "gpu_wready", "gpu_rvalid"] + [f"gpu_d[{i}]" for i in range(8)]

# Package-pin lookup through the bundled Apicula database; run under the suite
# Python because the system interpreter does not have Apicula installed.
PIN_CHECK = r'''
import json, sys, warnings
warnings.filterwarnings("ignore")
from apycula import chipdb
pins = chipdb.load_chipdb(sys.argv[1]).pinout["GW2AR-18C"]["QFN88"]
loc2pin = {loc: int(pin) for pin, (loc, cfg) in pins.items() if pin.isdigit()}
top = json.load(open(sys.argv[2]))["modules"]["top"]
xy = [c["attributes"]["NEXTPNR_BEL"].split("/")[0] for c in top["cells"].values()
      if "NEXTPNR_BEL" in c.get("attributes", {})]
W = max(int(s[1:].split("Y")[0]) for s in xy); H = max(int(s.split("Y")[1]) for s in xy)
bit2port = {b: (f"{p}[{i}]" if len(v["bits"]) > 1 else p)
            for p, v in top["ports"].items() for i, b in enumerate(v["bits"])}
result = {}
for c in top["cells"].values():
    bel = c.get("attributes", {}).get("NEXTPNR_BEL", "")
    if not bel.endswith(("/IOBA", "/IOBB")): continue
    x, y = (int(v) for v in bel.split("/")[0][1:].split("Y")); ab = bel[-1]
    side = "IOL" if x == 0 else "IOR" if x == W else "IOT" if y == 0 else "IOB"
    loc = f"{side}{(y if side in ('IOL', 'IOR') else x) + 1}{ab}"
    for k in ("I", "O", "IO"):
        for b in c["connections"].get(k, []):
            if b in bit2port: result[bit2port[b]] = {"bel": bel, "site": loc, "pin": loc2pin.get(loc)}
print(json.dumps(result))
'''


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    build = ROOT / "build/bus-echo"
    build.mkdir(parents=True, exist_ok=True)
    image = build / "bus-echo.fs"
    image.unlink(missing_ok=True)
    source = HERE / "tang20k_bus_echo.sv"
    board_cst = ROOT / "rtl/board/tang20k.cst"
    inputs = [source, HERE / "bus_echo_tb.sv", board_cst, Path(__file__).resolve(), ROOT / "scripts/rtl-check.py"]
    hashes = {p.relative_to(ROOT).as_posix(): digest(p) for p in inputs}
    report = {"schema": "N20K-BUS-ECHO-1", "device": DEVICE, "top": TOP, "source_sha256": hashes,
              "purpose": "Diagnostic host-bus echo; returns the byte latched at the last accepted address write",
              "gpu_image": False, "hardware_tested": False, "upload_ready": False,
              "timing_scope": "nextpnr internal estimate for gpu_hclk only; no external electrical margin"}

    def save():
        (build / "report.json").write_text(json.dumps(report, indent=2) + "\n", newline="\n")
    save()
    # Behavioral check first: no image unless the firmware sequences pass.
    HDL.run(HDL.tool("iverilog") + ["-g2012", "-s", "bus_echo_tb", "-o", str(build / "tb.vvp"),
            str(HERE / "bus_echo_tb.sv"), str(source)], build / "tb-compile.log")
    sim = HDL.run(HDL.tool("vvp") + ["-n", str(build / "tb.vvp")], build / "tb.log")
    if "PASS bus echo" not in sim: raise RuntimeError("bus echo simulation failed")
    report["simulation_passed"] = True; save()
    # Copy only this image's bus constraint lines, verbatim, from the GPU board file.
    lines = [l for l in board_cst.read_text().splitlines()
             if any(f'"{p}"' in l for p in PORTS)]
    found = {l.split('"')[1] for l in lines if l.startswith("IO_LOC")}
    if found != set(PORTS): raise RuntimeError("missing constraints: " + str(set(PORTS) - found))
    cst = build / "bus-echo.cst"
    cst.write_text("// Generated from rtl/board/tang20k.cst for the bus echo image.\n" + "\n".join(lines) + "\n", newline="\n")
    expected = {l.split('"')[1]: int(l.split()[-1].rstrip(";")) for l in lines if l.startswith("IO_LOC")}
    netlist = build / "synth.json"
    HDL.run(HDL.tool("yosys") + ["-Q", "-T", "-p", f"read_verilog -sv {json.dumps(str(source))}; "
            f"synth_gowin -family gw2a -top {TOP} -json {json.dumps(str(netlist))}; stat"], build / "synth.log")
    report["synthesized"] = True; save()
    extra = {"PYTHONHOME": str(HDL.SUITE)} if HDL.SUITE.is_dir() else {}
    command = HDL.tool("nextpnr-himbaechel") + ["--device", DEVICE, "-o", "family=GW2A-18C", "-o", "cst=" + str(cst),
        "--json", str(netlist), "--write", str(build / "routed.json"), "--report", str(build / "timing.json"),
        "--freq", "1", "--seed", "1"]
    db = HDL.SUITE / "share/nextpnr/himbaechel/gowin/chipdb-GW2A-18C.bin"
    if db.exists(): command += ["--chipdb", str(db)]
    HDL.run(command, build / "pnr.log", extra)
    report["routed"] = True
    timing = json.loads((build / "timing.json").read_text())
    report["fmax"] = timing.get("fmax", {})
    # The echo has only pad-to-register and register-to-pad paths, which
    # nextpnr does not report as an Fmax; any interior path must meet 1 MHz.
    no_paths = "no interior timing paths found in design" in (build / "pnr.log").read_text()
    report["interior_timing_paths"] = not no_paths
    report["timing_passed"] = (no_paths and not report["fmax"]) or (
        bool(report["fmax"]) and all(v["achieved"] >= v["constraint"] for v in report["fmax"].values()))
    save()
    if not report["timing_passed"]: raise RuntimeError("timing not met at 1 MHz")
    python = HDL.SUITE / ("lib/python3.exe" if sys.platform == "win32" else "bin/python3")
    chip = HDL.SUITE / "lib/python3.11/site-packages/apycula/GW2A-18C.msgpack.xz"
    placed = json.loads(subprocess.check_output([str(python), "-c", PIN_CHECK, str(chip), str(build / "routed.json")],
                                                env={**HDL.environment(), **extra}, text=True))
    report["pins"] = placed
    mismatched = [p for p in PORTS if placed.get(p, {}).get("pin") != expected[p]]
    report["pins_verified"] = not mismatched; save()
    if mismatched: raise RuntimeError("pin mismatch: " + ", ".join(mismatched))
    HDL.run(HDL.tool("gowin_pack") + ["-d", "GW2A-18C", "-c", "-o", str(image), str(build / "routed.json")],
            build / "bitstream.log", extra)
    if image.stat().st_size < 1000: raise RuntimeError("invalid short bitstream")
    changed = [n for n, h in hashes.items() if digest(ROOT / n) != h]
    if changed: raise RuntimeError("sources changed during build: " + ", ".join(changed))
    report.update(upload_ready=True, bitstream_sha256=digest(image), bitstream_bytes=image.stat().st_size)
    save()
    print("Bus echo image: " + str(image))
    print("SHA-256: " + report["bitstream_sha256"])


if __name__ == "__main__":
    try: main()
    except Exception as error:
        print("bus-echo build: " + str(error), file=sys.stderr); sys.exit(1)
