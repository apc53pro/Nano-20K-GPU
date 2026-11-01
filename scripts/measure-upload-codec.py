#!/usr/bin/env python3
"""Measure an isolated upload-word decoder against a raw-byte assembler.

This experiment does not integrate a new packet opcode or change production
RTL.  Exact-part placement/routing here qualifies only the registered-I/O
probe, not the complete GPU or a production board wrapper.
"""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def import_script(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HDL = import_script("upload_measure_hdl", "rtl-check.py")
PACK = import_script("upload_measure_pack", "rtl-core-pack.py")
PNR = import_script("upload_measure_pnr", "rtl-core-pnr.py")
DEVICE = "GW2AR-LV18QN88C8/I7"
FAMILY = "GW2A-18C"

# A one-byte/cycle raw upload assembler.  It has the same record framing,
# backpressure, output width/index and final-word handshake as the decoder.
# An output handshake can accept the first byte of the next word on that edge.
BASELINE = """module nano_upload_raw_baseline (
    input wire clk,reset,start,output wire start_ready,
    input wire s_valid,input wire [7:0] s_data,output wire s_ready,
    output reg m_valid,output reg [31:0] m_data,output reg [6:0] m_index,
    input wire m_ready,output reg busy,done
);
    reg [1:0] byte_index;
    assign start_ready=!busy;
    assign s_ready=busy&&(!m_valid||m_ready)&&!(m_valid&&m_ready&&m_index==79);
    always @(posedge clk) begin
        if(reset)begin
            busy<=0;done<=0;m_valid<=0;m_data<=0;m_index<=0;byte_index<=0;
        end else begin
            done<=0;
            if(start&&start_ready)begin
                busy<=1;m_valid<=0;m_data<=0;m_index<=0;byte_index<=0;
            end else if(busy)begin
                if(m_valid&&m_ready)begin
                    m_valid<=0;
                    if(m_index==79)begin busy<=0;done<=1;end
                    else m_index<=m_index+1;
                end
                if(s_valid&&s_ready)begin
                    case(byte_index)
                        0:m_data[7:0]<=s_data;
                        1:m_data[15:8]<=s_data;
                        2:m_data[23:16]<=s_data;
                        3:m_data[31:24]<=s_data;
                    endcase
                    byte_index<=byte_index+1;
                    if(byte_index==3)m_valid<=1;
                end
            end
        end
    end
endmodule
"""

PROBE = """// Qualification probe only; package IO is not a wiring layout.
module nano_upload_measure_probe (
    input wire clk,reset,stimulus_shift,serial_data,capture_outputs,output_shift,
    output wire serial_result,core_ready,core_done
);
    // Equal stimulus/capture overhead in both variants. Every input can be
    // selected through the serial stimulus and every output bit captured and
    // shifted out. The core therefore retains all datapath/handshake cones.
    reg [11:0] stimulus;
    reg [43:0] snapshot;
    wire core_start_ready,core_s_ready,core_m_valid,core_busy,core_done;
    wire [31:0] core_m_data;
    wire [6:0] core_m_index;
    always @(posedge clk)begin
        if(reset)begin stimulus<=0;snapshot<=0;end
        else begin
            if(stimulus_shift)stimulus<={stimulus[10:0],serial_data};
            if(capture_outputs)snapshot<={core_start_ready,core_s_ready,
                core_m_valid,core_busy,core_done,core_m_index,core_m_data};
            else if(output_shift)snapshot<={snapshot[42:0],1'b0};
        end
    end
    assign serial_result=snapshot[43];
    assign core_ready=core_s_ready;
    CORE_MODULE core(.clk(clk),.reset(reset|stimulus[11]),.start(stimulus[10]),
        .start_ready(core_start_ready),.s_valid(stimulus[9]),.s_data(stimulus[7:0]),
        .s_ready(core_s_ready),.m_valid(core_m_valid),.m_data(core_m_data),
        .m_index(core_m_index),.m_ready(stimulus[8]),.busy(core_busy),.done(core_done));
endmodule
"""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, logfile=None, extra_env=None):
    return HDL.run(command, logfile=logfile, extra_env=extra_env)


def counts(netlist, top):
    return dict(sorted(Counter(c["type"] for c in netlist["modules"][top]["cells"].values()).items()))


def json_write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/upload-codec-measurement")
    parser.add_argument("--report", type=Path, help="Optional compact report destination; default is build/qualification.json")
    parser.add_argument("--no-route", action="store_true", help="Synthesize and pack only; no timing evidence")
    parser.add_argument("--target-mhz", type=float, default=68.88)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    if not 0 < args.target_mhz < 1000:
        raise ValueError("target MHz must be positive and less than 1000")
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    inputs = [ROOT / p for p in ("experiments/upload-codec/nano_upload_word_decoder.sv",
        "scripts/measure-upload-codec.py", "scripts/rtl-check.py", "scripts/rtl-core-pack.py", "scripts/rtl-core-pnr.py")]
    source_hashes = {p.relative_to(ROOT).as_posix(): digest(p) for p in inputs}
    extra = {"PYTHONHOME": str(HDL.SUITE)} if HDL.SUITE.is_dir() else {}
    report = {"schema": "N20K-UPLOAD-CODEC-PROBE-1", "device": DEVICE, "family": FAMILY,
        "scope": "Isolated decoder and raw assembler in identical serial registered-I/O probes; no production GPU integration",
        "production_protocol_changed": False, "whole_gpu_fit_qualified": False,
        "board_pinout_qualified": False, "real_hardware_measured": False,
        "target_mhz": args.target_mhz, "seed": args.seed,
        "probe_register_bits_before_optimization": 56,
        "synthesis_options": ["-nowidelut"], "source_sha256": source_hashes,
        "yosys_version": run(HDL.tool("yosys") + ["-V"]).strip(),
        "nextpnr_version": run(HDL.tool("nextpnr-himbaechel") + ["--version"], extra_env=extra).strip(),
        "variants": {}}
    decoder = build / "nano_upload_word_decoder.sv"
    decoder.write_bytes(inputs[0].read_bytes())
    baseline = build / "raw_baseline.sv"
    baseline.write_text(BASELINE, encoding="utf-8")
    cst = build / "probe.cst"
    probe_pins = {"clk": 4, "reset": 88, "stimulus_shift": 15,
        "serial_data": 16, "capture_outputs": 17, "output_shift": 18,
        "serial_result": 19, "core_ready": 20, "core_done": 87}
    cst.write_text("// Isolated serial qualification probe; not board wiring.\n" +
        "".join('IO_LOC "{}" {};\nIO_PORT "{}" IO_TYPE=LVCMOS33;\n'.format(name, pin, name)
            for name, pin in probe_pins.items()), encoding="utf-8")
    for variant, module in (("raw_baseline", "nano_upload_raw_baseline"), ("decoder", "nano_upload_word_decoder")):
        selected = build / variant
        selected.mkdir(exist_ok=True)
        wrapper = selected / "probe.sv"
        wrapper.write_text(PROBE.replace("CORE_MODULE", module), encoding="utf-8")
        sources = [decoder, baseline, wrapper]
        core_netlist = selected / "core-synth.json"
        core_files = " ".join(json.dumps(str(p)) for p in (decoder, baseline))
        core_command = "read_verilog -sv {}; synth_gowin -family gw2a -top {} -nowidelut -noiopads -json {}; stat".format(
            core_files, module, json.dumps(str(core_netlist)))
        run(HDL.tool("yosys") + ["-Q", "-T", "-p", core_command], selected / "core-synthesis.log")
        core_counts = counts(json.loads(core_netlist.read_text()), module)
        netlist = selected / "synth.json"
        files = " ".join(json.dumps(str(p)) for p in sources)
        command = "read_verilog -sv {}; synth_gowin -family gw2a -top nano_upload_measure_probe -nowidelut -json {}; stat".format(files, json.dumps(str(netlist)))
        print("Synthesizing " + variant + "...", flush=True)
        run(HDL.tool("yosys") + ["-Q", "-T", "-p", command], selected / "synthesis.log")
        variant_report = {"synthesized": True, "synthesis_cells": counts(json.loads(netlist.read_text()), "nano_upload_measure_probe"),
            "core_synthesis_cells": core_counts,
            "core_mapped_summary": {"LUT1_to_LUT4": sum(core_counts.get("LUT" + str(i), 0) for i in range(1, 5)),
                "ALU": core_counts.get("ALU", 0), "DFF": sum(n for k, n in core_counts.items() if k.startswith("DFF")),
                "BSRAM": sum(core_counts.get(k, 0) for k in ("SDPB", "SDPX9B", "DPB", "SP", "ROM")),
                "MULT18X18": core_counts.get("MULT18X18", 0)},
            "core_synth_netlist_sha256": digest(core_netlist),
            "synth_netlist_sha256": digest(netlist), "probe_sha256": digest(wrapper),
            "placed": False, "routed": False, "timing_qualified": False}
        common = HDL.tool("nextpnr-himbaechel") + ["--device", DEVICE, "-o", "family=" + FAMILY,
            "-o", "cst=" + str(cst), "--json", str(netlist), "--seed", str(args.seed), "--threads", "2"]
        database = HDL.SUITE / "share/nextpnr/himbaechel/gowin/chipdb-GW2A-18C.bin"
        if database.is_file():
            common += ["--chipdb", str(database)]
        packed = selected / "packed.json"
        run(common + ["--pack-only", "--write", str(packed)], selected / "pack.log", extra)
        packed_cells = json.loads(packed.read_text())["modules"]["top"]["cells"]
        variant_report.update(packed=True, packed_cells=counts(json.loads(packed.read_text()), "top"),
            physical_lut_lower_bound=PACK.physical_lut_lower_bound(packed_cells))
        if not args.no_route:
            print("Routing " + variant + " (probe only)...", flush=True)
            routed, timing = selected / "routed.json", selected / "timing.json"
            run(common + ["--write", str(routed), "--freq", str(args.target_mhz), "--timing-allow-fail",
                "--report", str(timing), "--detailed-timing-report"], selected / "pnr.log", extra)
            design = json.loads(routed.read_text())["modules"]["top"]
            if not PNR.setting(design.get("settings", {}).get("place")) or not PNR.setting(design.get("settings", {}).get("route")):
                raise RuntimeError("No completed placement and routing settings for " + variant)
            timing_json = json.loads(timing.read_text())
            fmax = timing_json.get("fmax", {})
            if len(fmax) != 1:
                raise RuntimeError("Expected exactly one clock in isolated probe timing report")
            clock, value = next(iter(fmax.items()))
            achieved, constraint = value.get("achieved"), value.get("constraint")
            if not isinstance(achieved, (int, float)) or not isinstance(constraint, (int, float)) or achieved <= 0 or constraint <= 0:
                raise RuntimeError("Missing positive timing frequencies")
            resources = PNR.physical_resources(design["cells"])
            within_capacity = all(value["within_capacity"] for value in resources.values())
            target_met = achieved >= args.target_mhz
            variant_report.update(placed=True, routed=True, timing_qualified=target_met and within_capacity,
                post_route_physical_resources=resources,
                post_route_resources_within_capacity=within_capacity,
                fmax={clock: {"achieved": achieved, "constraint": constraint}},
                target_met=target_met,
                critical_paths=PNR.critical_paths(timing_json),
                routed_netlist_sha256=digest(routed), timing_report_sha256=digest(timing))
        report["variants"][variant] = variant_report
        print(json.dumps({"variant": variant, "resources": variant_report.get("post_route_physical_resources"),
            "fmax": variant_report.get("fmax")}, indent=2), flush=True)
    if not args.no_route:
        variants = report["variants"]
        report["decoder_minus_raw_probe_resources"] = {name: variants["decoder"]["post_route_physical_resources"][name]["used"] -
            variants["raw_baseline"]["post_route_physical_resources"][name]["used"] for name in PNR.CAPACITIES}
    report["inputs_changed_during_run"] = [name for name, sha in source_hashes.items() if digest(ROOT / name) != sha]
    if report["inputs_changed_during_run"]:
        raise RuntimeError("Measurement inputs changed during run: " + ", ".join(report["inputs_changed_during_run"]))
    report["generated_input_sha256"] = {p.relative_to(build).as_posix(): digest(p) for p in (decoder, baseline, cst)}
    json_write(build / "qualification.json", report)
    if args.report:
        compact = dict(report)
        compact["variants"] = {name: {k: v for k, v in result.items() if k not in ("synthesis_cells", "packed_cells")}
            for name, result in report["variants"].items()}
        json_write(args.report.resolve(), compact)
    print("Qualification report: " + str(build / "qualification.json"))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print("measure-upload-codec: " + str(error), file=sys.stderr)
        sys.exit(1)
