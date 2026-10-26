"""Gowin FF input LUTs must not disappear from the physical resource gate."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gpu_pack_physical_bound", ROOT / "scripts/rtl-core-pack.py")
pack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pack)


def logic(kind, port, bit):
    return {"type": kind, "port_directions": {port: "output"}, "connections": {port: [bit]}}


def ff(bit):
    return {"type": "DFFE", "port_directions": {"D": "input"}, "connections": {"D": [bit]}}


class PhysicalLutBoundTests(unittest.TestCase):
    def test_own_lut_and_alu_outputs_can_supply_one_ff_each(self):
        cells = {"a": logic("LUT4", "F", 1), "b": logic("ALU", "SUM", 2),
                 "alu_site": {"type": "BLOCKER_LUT"}, "ff1": ff(1), "ff2": ff(2)}
        result = pack.physical_lut_lower_bound(cells)
        self.assertEqual(result["maximum_existing_lut_alu_pairs"], 2)
        self.assertEqual(result["minimum_additional_ff_input_luts"], 0)
        self.assertEqual(result["minimum_physical_lut_sites"], 2)

    def test_duplicate_data_registers_need_an_additional_site(self):
        result = pack.physical_lut_lower_bound({"a": logic("LUT2", "F", 1), "ff1": ff(1), "ff2": ff(1)})
        self.assertEqual(result["minimum_additional_ff_input_luts"], 1)
        self.assertEqual(result["minimum_physical_lut_sites"], 2)

    def test_register_chains_and_native_outputs_need_feed_luts(self):
        cells = {"reg": logic("DFFE", "Q", 1), "ram": logic("SDP", "DO", 2),
                 "ff1": ff(1), "ff2": ff(2)}
        cells["reg"]["connections"]["D"] = ["0"]
        cells["reg"]["port_directions"]["D"] = "input"
        result = pack.physical_lut_lower_bound(cells)
        self.assertEqual(result["active_flipflops"], 3)
        self.assertEqual(result["minimum_additional_ff_input_luts"], 3)

    def test_wide_mux_and_carry_output_are_not_lut_f_or_alu_sum(self):
        cells = {"mux": logic("MUX2_LUT5", "OF", 1), "alu": logic("ALU", "COUT", 2),
                 "ff1": ff(1), "ff2": ff(2)}
        self.assertEqual(pack.physical_lut_lower_bound(cells)["maximum_existing_lut_alu_pairs"], 0)

    def test_blockers_count_as_sites_but_not_active_flipflops(self):
        result = pack.physical_lut_lower_bound({"reserved": {"type": "BLOCKER_LUT"},
                                             "ff_reserved": {"type": "BLOCKER_FF"}})
        self.assertEqual(result["active_flipflops"], 0)
        self.assertEqual(result["minimum_physical_lut_sites"], 1)

    def test_packed_totals_can_pass_while_physical_lower_bound_fails(self):
        result = pack.physical_lut_lower_bound({"lut": logic("LUT4", "F", 1), "ff": ff("0")}, capacity=1)
        self.assertEqual(result["packed_lut_sites"], 1)
        self.assertEqual(result["minimum_physical_lut_sites"], 2)
        self.assertFalse(result["within_capacity"])


if __name__ == "__main__":
    unittest.main()
