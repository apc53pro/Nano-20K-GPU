#!/usr/bin/env python3
"""Measure cold and steady system-model submissions with unchanged scenes.

The first submission can include SDRAM/device boot. A separate two-frame run
provides the last, boot-free submission and later-frame traffic deltas. The CLI
waits for presentation and a full scanout observation after every frame; those
waits are reported separately and are not included in submission_fps.
"""

import argparse
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path


SCENES = ("showcase", "perspective", "overdraw", "opaque")
COUNTERS = {
    "uploaded_bytes": ("system", "uploaded_bytes"),
    "host_rx_bytes": ("system", "host", "rx_bytes"),
    "host_tx_bytes": ("system", "host", "tx_bytes"),
    "host_backpressure_edges": ("system", "host", "backpressure_edges"),
    "fetched_record_bytes": ("system", "fetched_record_bytes"),
    "fetched_bin_bytes": ("system", "fetched_bin_bytes"),
}


def nested(document, path):
    for key in path:
        document = document[key]
    return document


def summarize(document, previous=None):
    system = document["system"]
    core_hz = system["configured_core_hz"]
    pass_cycles = system["last_pass_cycles"]
    submission_cycles = system["last_submission_cycles"]
    if core_hz <= 0 or pass_cycles <= 0 or submission_cycles < pass_cycles:
        raise ValueError("invalid measured core/pass/submission cycles")
    divisor = document["frames"] - previous["frames"] if previous else 1
    if divisor <= 0:
        raise ValueError("steady run must contain more frames than the cold run")
    traffic = {}
    for name, path in COUNTERS.items():
        value = nested(document, path) - (nested(previous, path) if previous else 0)
        if value < 0:
            raise ValueError("cumulative counter decreased: " + name)
        traffic[name] = value / divisor
    traffic["total_host_bytes"] = traffic["host_rx_bytes"] + traffic["host_tx_bytes"]
    # With exactly one subsequent frame, subtract the independently repeated
    # first frame and that second frame's diagnostic scanout wait. This retains
    # real PRESENT transfer/reply and vblank waiting, unlike submission_fps.
    driver_frame_cycles = None
    if previous and divisor == 1:
        driver_frame_cycles = (system["core_cycles"] - previous["system"]["core_cycles"] -
                               system["last_scanout_observation_cycles"])
        if driver_frame_cycles < submission_cycles:
            raise ValueError("driver frame elapsed time is shorter than its submission")
    memory = system["sdram"]
    if memory["legacy_submissions"] or memory["legacy_response_reads"]:
        raise ValueError("system workload used an untimed legacy SDRAM adapter")
    if system["host"]["protocol_faults"]:
        raise ValueError("host protocol fault during benchmark")
    if system["scanout"]["underflows"]:
        raise ValueError("display FIFO underflow during benchmark")
    return {
        "last_pass_cycles": pass_cycles,
        "last_submission_cycles": submission_cycles,
        "last_pass_ms": 1000 * pass_cycles / core_hz,
        "last_submission_ms": 1000 * submission_cycles / core_hz,
        "submission_fps": core_hz / submission_cycles,
        "submission_includes_boot": previous is None,
        "boot_cycles": system["boot_cycles"],
        "last_present_wait_cycles": system["last_present_wait_cycles"],
        "last_scanout_observation_cycles": system["last_scanout_observation_cycles"],
        "driver_frame_cycles_excluding_scanout_observation": driver_frame_cycles,
        "driver_frame_ms_excluding_scanout_observation":
            1000 * driver_frame_cycles / core_hz if driver_frame_cycles is not None else None,
        "driver_frame_fps_excluding_scanout_observation":
            core_hz / driver_frame_cycles if driver_frame_cycles is not None else None,
        "mean_traffic_per_frame": traffic,
        "last_frame": document["last_frame"],
        "color_fnv1a64": document["color_fnv1a64"],
        "depth_fnv1a64": document["depth_fnv1a64"],
        "verified_against_other_modes": document["verified_against_other_modes"],
        "scanout_underflows": system["scanout"]["underflows"],
    }


def run_scene(executable, scene, frames, args, directory):
    stats_path = directory / (scene + "-" + str(frames) + ".json")
    command = [str(executable), "--headless", "--mode", "system", "--scene", scene,
               "--frames", str(frames), "--time", "0", "--step", "0",
               "--stats", str(stats_path)]
    if args.verify:
        command.append("--verify")
    if args.host_divider is not None:
        command.extend(("--host-divider", str(args.host_divider)))
    completed = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
    if completed.returncode:
        raise RuntimeError("renderer failed for " + scene + ": " + completed.stderr.strip())
    with stats_path.open(encoding="utf-8") as source:
        document = json.load(source)
    if document["scene"] != scene or document["frames"] != frames or document["mode"] != "system":
        raise ValueError("renderer reported a different workload")
    if args.verify and not document["verified_against_other_modes"]:
        raise ValueError("renderer did not perform requested oracle verification")
    return document


def compare(result, baseline, maximum_ratio):
    previous = {item["scene"]: item for item in baseline["results"]}
    regressions = []
    for item in result["results"]:
        if item["scene"] not in previous:
            continue
        old = previous[item["scene"]]
        if item["configured_core_hz"] != old["configured_core_hz"] or \
                item["configured_host_hz"] != old["configured_host_hz"]:
            raise ValueError("baseline clocks differ for " + item["scene"])
        for phase in ("cold", "steady"):
            current = item[phase]
            earlier = old[phase]
            for key in ("color_fnv1a64", "depth_fnv1a64"):
                if current[key] != earlier[key]:
                    raise ValueError(item["scene"] + " " + phase + " " + key + " differs from baseline")
            ratios = {}
            for key in ("last_pass_cycles", "last_submission_cycles"):
                ratios[key] = current[key] / earlier[key]
                if ratios[key] > maximum_ratio:
                    regressions.append(item["scene"] + " " + phase + " " + key)
            current["baseline_cycle_ratios"] = ratios
    return regressions


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--executable", type=Path, default=root / "build" / "nano-gpu-ref-headless")
    parser.add_argument("--scenes", nargs="+", choices=SCENES, default=list(SCENES))
    parser.add_argument("--output", type=Path, default=root / "build" / "system-benchmark.json")
    parser.add_argument("--baseline", type=Path, help="compare matching workloads and require identical hashes")
    parser.add_argument("--steady-frames", type=int, default=2,
                        help="frozen-scene run length; report its final submission (default: 2)")
    parser.add_argument("--verify", action="store_true", help="compare every rendered frame with canonical modes")
    parser.add_argument("--host-divider", type=int)
    parser.add_argument("--target-fps", type=float, default=30)
    parser.add_argument("--require-target", action="store_true", help="fail if a steady submission misses target FPS")
    parser.add_argument("--require-no-regression", action="store_true")
    parser.add_argument("--maximum-cycle-ratio", type=float, default=1.05,
                        help="allowed measured/baseline cycle ratio (default: 1.05)")
    parser.add_argument("--timeout", type=float, default=300, help="timeout in seconds per renderer process")
    args = parser.parse_args()
    positive_values = (args.target_fps, args.maximum_cycle_ratio, args.timeout)
    if args.steady_frames < 2 or any(not math.isfinite(value) or value <= 0 for value in positive_values):
        parser.error("steady frames must be at least two; FPS, cycle ratio and timeout must be positive")
    if args.require_no_regression and not args.baseline:
        parser.error("--require-no-regression requires --baseline")
    executable = args.executable.resolve()
    result = {
        "schema": "N20K-SYSTEM-BENCHMARK-1",
        "scope": "submission includes uploads and rendering, excludes PRESENT/vblank; two-frame driver metric includes PRESENT/vblank but excludes diagnostic scanout observation; neither measures RP2350 CPU execution or achieved FPGA timing",
        "physical_performance_validated": False,
        "animation_time": 0,
        "animation_step": 0,
        "steady_frames": args.steady_frames,
        "target_fps": args.target_fps,
        "results": [],
    }
    with tempfile.TemporaryDirectory(prefix="nano-system-benchmark-") as temporary:
        for scene in args.scenes:
            print("Measuring " + scene + " (cold and steady)", file=sys.stderr, flush=True)
            cold = run_scene(executable, scene, 1, args, Path(temporary))
            later = run_scene(executable, scene, args.steady_frames, args, Path(temporary))
            system = later["system"]
            item = {
                "scene": scene,
                "configured_core_hz": system["configured_core_hz"],
                "configured_host_hz": system["configured_host_hz"],
                "configured_pixel_hz": system["configured_pixel_hz"],
                "cold": summarize(cold),
                "steady": summarize(later, cold),
            }
            if item["cold"]["color_fnv1a64"] != item["steady"]["color_fnv1a64"] or \
                    item["cold"]["depth_fnv1a64"] != item["steady"]["depth_fnv1a64"]:
                raise ValueError(scene + " frozen-scene output changed between cold and steady runs")
            item["steady"]["meets_submission_target"] = item["steady"]["submission_fps"] >= args.target_fps
            frame_fps = item["steady"]["driver_frame_fps_excluding_scanout_observation"]
            item["steady"]["meets_driver_frame_target"] = \
                frame_fps >= args.target_fps if frame_fps is not None else None
            result["results"].append(item)
    regressions = []
    if args.baseline:
        with args.baseline.open(encoding="utf-8") as source:
            baseline = json.load(source)
        if baseline.get("schema") != result["schema"]:
            raise ValueError("unsupported baseline schema")
        regressions = compare(result, baseline, args.maximum_cycle_ratio)
    result["cycle_regressions"] = regressions
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    for item in result["results"]:
        steady = item["steady"]
        print("{}: pass {:.3f} ms, submission {:.3f} ms ({:.2f} FPS), host {:.0f} bytes/frame{}".format(
            item["scene"], steady["last_pass_ms"], steady["last_submission_ms"], steady["submission_fps"],
            steady["mean_traffic_per_frame"]["total_host_bytes"],
            " [target met]" if steady["meets_submission_target"] else " [target missed]"))
    print("Saved " + str(args.output))
    if args.require_no_regression and regressions:
        raise RuntimeError("cycle regression: " + ", ".join(regressions))
    if args.require_target and any(not item["steady"]["meets_submission_target"] for item in result["results"]):
        raise RuntimeError("one or more steady submissions missed the target FPS")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.TimeoutExpired) as error:
        print("benchmark-system: " + str(error), file=sys.stderr)
        sys.exit(1)
