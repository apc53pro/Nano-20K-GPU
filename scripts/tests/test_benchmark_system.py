"""Check benchmark accounting independently of renderer execution speed."""

import copy
import importlib.util
from pathlib import Path
import unittest


MODULE = Path(__file__).resolve().parents[1] / "benchmark-system.py"
SPEC = importlib.util.spec_from_file_location("benchmark_system", MODULE)
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


def report(frames=1):
    return {
        "frames": frames,
        "verified_against_other_modes": True,
        "last_frame": {"covered_fragments": 1024},
        "color_fnv1a64": "1234",
        "depth_fnv1a64": "5678",
        "system": {
            "configured_core_hz": 1000000,
            "core_cycles": 16000,
            "last_pass_cycles": 5000,
            "last_submission_cycles": 10000,
            "last_present_wait_cycles": 900,
            "last_scanout_observation_cycles": 5000,
            "boot_cycles": 4000,
            "uploaded_bytes": 800,
            "fetched_record_bytes": 640,
            "fetched_bin_bytes": 2436,
            "host": {"rx_bytes": 840, "tx_bytes": 56, "backpressure_edges": 1000,
                     "protocol_faults": 0},
            "sdram": {"legacy_submissions": 0, "legacy_response_reads": 0},
            "scanout": {"underflows": 0},
        },
    }


class AccountingTests(unittest.TestCase):
    def test_cold_boot_is_visible_and_not_claimed_as_steady_rate(self):
        cold = benchmark.summarize(report())
        self.assertTrue(cold["submission_includes_boot"])
        self.assertEqual(cold["boot_cycles"], 4000)
        self.assertEqual(cold["last_submission_ms"], 10)
        self.assertIsNone(cold["driver_frame_fps_excluding_scanout_observation"])

    def test_second_frame_uses_counter_deltas_and_keeps_presentation_cost(self):
        cold = report()
        later = copy.deepcopy(cold)
        later["frames"] = 2
        later["system"].update(core_cycles=27500, last_submission_cycles=6000,
                              uploaded_bytes=1000, fetched_record_bytes=1280,
                              fetched_bin_bytes=4872)
        later["system"]["host"].update(rx_bytes=1080, tx_bytes=112, backpressure_edges=1010)
        steady = benchmark.summarize(later, cold)
        self.assertFalse(steady["submission_includes_boot"])
        self.assertEqual(steady["mean_traffic_per_frame"]["uploaded_bytes"], 200)
        self.assertEqual(steady["mean_traffic_per_frame"]["total_host_bytes"], 296)
        self.assertEqual(steady["mean_traffic_per_frame"]["host_backpressure_edges"], 10)
        self.assertEqual(steady["last_submission_ms"], 6)
        self.assertEqual(steady["driver_frame_cycles_excluding_scanout_observation"], 6500)
        self.assertEqual(steady["driver_frame_ms_excluding_scanout_observation"], 6.5)

    def test_unobservable_last_frame_elapsed_is_not_inferred_for_longer_runs(self):
        cold = report()
        later = copy.deepcopy(cold)
        later["frames"] = 3
        later["system"]["uploaded_bytes"] = 1200
        steady = benchmark.summarize(later, cold)
        self.assertEqual(steady["mean_traffic_per_frame"]["uploaded_bytes"], 200)
        self.assertIsNone(steady["driver_frame_cycles_excluding_scanout_observation"])

    def test_underflow_or_untimed_adapter_cannot_pass_benchmark(self):
        for group, key in (("scanout", "underflows"), ("sdram", "legacy_submissions"),
                           ("sdram", "legacy_response_reads"), ("host", "protocol_faults")):
            with self.subTest(counter=key):
                invalid = report()
                invalid["system"][group][key] = 1
                with self.assertRaises(ValueError):
                    benchmark.summarize(invalid)

    def test_baseline_rejects_different_pixels_even_when_faster(self):
        phase = {"last_pass_cycles": 5000, "last_submission_cycles": 6000,
                 "color_fnv1a64": "1234", "depth_fnv1a64": "5678"}
        result = {"results": [{"scene": "showcase", "configured_core_hz": 1000000,
                               "configured_host_hz": 250000,
                               "cold": copy.deepcopy(phase), "steady": copy.deepcopy(phase)}]}
        baseline = copy.deepcopy(result)
        result["results"][0]["steady"].update(last_pass_cycles=4000, depth_fnv1a64="wrong")
        with self.assertRaises(ValueError):
            benchmark.compare(result, baseline, 1.05)

    def test_baseline_reports_relative_regression_without_exact_cycle_lock(self):
        phase = {"last_pass_cycles": 5000, "last_submission_cycles": 6000,
                 "color_fnv1a64": "1234", "depth_fnv1a64": "5678"}
        baseline = {"results": [{"scene": "showcase", "configured_core_hz": 1000000,
                                 "configured_host_hz": 250000,
                                 "cold": copy.deepcopy(phase), "steady": copy.deepcopy(phase)}]}
        result = copy.deepcopy(baseline)
        result["results"][0]["steady"]["last_pass_cycles"] = 5300
        self.assertEqual(benchmark.compare(result, baseline, 1.05), ["showcase steady last_pass_cycles"])


if __name__ == "__main__":
    unittest.main()
