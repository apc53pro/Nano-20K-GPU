#!/usr/bin/env python3
"""Render documentation-only, printable wiring maps from the pinout JSON.

Run from any directory. Inkscape is used only to export the generated SVGs to PDF.
This script does not generate firmware, FPGA constraints, or electrical sign-off.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from html import escape
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PINOUT = ROOT / "docs/hardware/rp2350-tang20k-pinout.json"
IMAGES = ROOT / "docs/images"

INK = "#16283d"
MUTED = "#536174"
GRID = "#d4dce5"
BLUE = "#165bb0"
TEAL = "#00786f"
VIOLET = "#7543a2"
POWER = "#b85013"
GREY = "#77818d"


class SVG:
    def __init__(self, width: int, height: int, mm_width: int, mm_height: int, title: str):
        self.width = width
        self.height = height
        self.parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{mm_width}mm" '
            f'height="{mm_height}mm" viewBox="0 0 {width} {height}" '
            f'role="img" aria-label="{escape(title, quote=True)}">',
            f"<title>{escape(title)}</title>",
            '<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" '
            'markerWidth="7" markerHeight="7" orient="auto-start-reverse" '
            'markerUnits="strokeWidth"><path d="M0 0L10 5L0 10Z" fill="context-stroke"/>'
            '</marker></defs>',
            '<style>text{font-family:"DejaVu Sans",Arial,sans-serif;fill:#16283d}'
            '.mono{font-family:"DejaVu Sans Mono",monospace}'
            '</style>',
        ]
        self.rect(0, 0, width, height, fill="white", stroke="none")

    def rect(self, x, y, w, h, *, fill="white", stroke=GRID, radius=0, sw=1):
        self.parts.append(
            f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>'
        )

    def text(self, x, y, value, *, size=20, weight=400, color=INK, anchor="start", cls=""):
        self.parts.append(
            f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{weight}" '
            f'style="fill:{color}" text-anchor="{anchor}" class="{cls}">'
            f'{escape(str(value))}</text>'
        )

    def line(self, x1, y1, x2, y2, *, color=GRID, sw=1, start=False, end=False, dash=False):
        attrs = (' marker-start="url(#arrow)"' if start else "")
        attrs += (' marker-end="url(#arrow)"' if end else "")
        attrs += (' stroke-dasharray="6 5"' if dash else "")
        self.parts.append(
            f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" '
            f'stroke="{color}" stroke-width="{sw}"{attrs}/>'
        )

    def circle(self, x, y, r, *, fill="white", stroke=INK, sw=2):
        self.parts.append(
            f'<circle cx="{x}" cy="{y}" r="{r}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{sw}"/>'
        )

    def resistor(self, x, y, *, color):
        self.rect(x, y - 9, 62, 18, stroke=color, sw=2)
        self.text(x + 31, y + 5, "100Ω", size=15, weight=600, color=color, anchor="middle")

    def save(self, path: Path):
        path.write_text("\n".join(self.parts + ["</svg>"]) + "\n", encoding="utf-8")


def color_for_signal(signal: str):
    if signal.startswith("D") and signal[1:].isdigit():
        return VIOLET
    if signal in ("WREADY", "RVALID"):
        return TEAL
    return BLUE


def header(svg, title, subtitle):
    svg.text(55, 60, title, size=31, weight=700)
    svg.text(55, 96, subtitle, size=20, color=MUTED)
    svg.rect(55, 117, svg.width - 110, 63, fill="#fff5e8", stroke="#e6be83", radius=4)
    svg.text(73, 142, "Bus-test build: 6.3MHz GPU; black HDMI only; electrical timing unqualified", size=20, weight=700)
    svg.text(73, 166, "RP2350 A2; one 100Ω per signal wire; two 4.7kΩ handshake pull-downs fitted", size=19, weight=600, color=POWER)


def wiring(rows):
    s = SVG(1680, 1188, 297, 210, "Core2350B to Tang Nano20K: revised 16-signal point-to-point wiring")
    header(s, "Core2350B ↔ Tang Nano20K · 16-signal wiring map", "Core2350B: RP2350B / 8 MB PSRAM / 16 MB flash   •   Tang Nano20K hardware 3921")

    s.text(72, 213, "CORE2350B", size=20, weight=700)
    s.text(642, 213, "CABLE / SIGNAL FLOW", size=20, weight=700)
    s.text(1152, 213, "TANG NANO20K", size=20, weight=700)
    s.rect(55, 225, 1570, 583, fill="white", stroke=GRID)
    s.rect(55, 225, 1570, 44, fill="#edf2f7", stroke=GRID)
    headings = [(73, "GPIO"), (205, "Header pin"), (385, "Signal"), (666, "Single series resistor"), (968, "Flow"), (1154, "Header pin"), (1360, "FPGA pin"), (1510, "Bank")]
    for x, label in headings:
        s.text(x, 254, label, size=18, weight=700)
    for i, row in enumerate(rows):
        y = 287 + i * 33.5
        color = color_for_signal(row["signal"])
        if i % 2 == 0:
            s.rect(56, y - 16, 1568, 33.5, fill="#f8fafc", stroke="none")
        s.text(73, y + 6, f'GP{row["gpio"]}', size=20, cls="mono")
        s.text(205, y + 6, row["core_header"], size=20, weight=600, cls="mono")
        s.text(385, y + 6, row["signal"], size=20, weight=700, color=color, cls="mono")
        s.line(579, y, 1114, y, color=color, sw=2.6)
        s.circle(579, y, 3.5, fill=color, stroke=color, sw=1)
        s.circle(1114, y, 3.5, fill=color, stroke=color, sw=1)
        # Schematic position only: the user has not reported which end has R.
        s.resistor(724, y, color=color)
        if row["direction"] == "core_to_tang":
            s.line(941, y, 1049, y, color=color, sw=2.6, end=True)
        elif row["direction"] == "tang_to_core":
            s.line(941, y, 1049, y, color=color, sw=2.6, start=True)
        else:
            s.line(941, y, 1049, y, color=color, sw=2.6, start=True, end=True)
        s.text(1154, y + 6, row["tang_header"], size=20, weight=600, cls="mono")
        s.text(1375, y + 6, row["fpga_pin"], size=20, cls="mono")
        s.text(1535, y + 6, row["bank"], size=20, cls="mono")

    # Notes sit on the same printed sheet as the exact endpoint map.
    for x, w in [(55, 500), (572, 510), (1099, 526)]:
        s.rect(x, 831, w, 244, fill="#f8fafc", stroke=GRID, radius=4)
    s.text(73, 862, "Power and ground", size=21, weight=700)
    notes = [
        "Power boards through their normal inputs.",
        "Join common GND: Tang J6.20, J5.2, J5.15",
        "to Core P1.1 / P3.1 (fan-out as needed).",
        "Keep inter-board 3.3V and 5V rails separate.",
        "Tang J6.19: local pull-up rail only.",
        "Tang J5.1 (5V), J5.16 (3.3V): unconnected.",
    ]
    for i, note in enumerate(notes):
        s.text(73, 893 + 28 * i, note, size=18)
    s.text(590, 862, "Bias · fitted / recommended", size=21, weight=700)
    notes = [
        "Fitted: 4.7kΩ each GP39 / GP40 → GND.",
        "Added pulls: capability reads still all zero.",
        "Other external bias parts: not confirmed.",
        "Design: up WR_N, RD_N, ALE_N to Tang 3.3V;",
        "down HCLK, ABORT, RESET_N to GND at Tang.",
        "No data pulls recommended. RESET_N low = reset.",
    ]
    for i, note in enumerate(notes):
        s.text(590, 893 + 28 * i, note, size=18)
    s.text(1117, 862, "CONFIRMED assembly / shared pads", size=21, weight=700)
    notes = [
        "16 × 100Ω total; ONE per wire, including data.",
        "Resistor end placement was not reported.",
        "Disconnect the Tang FPC LCD and VGA DAC.",
        "Preserve HDMI CEC/HPD (FPGA pins 25/26).",
        "3 loads (15/16/17): orange LED + 510Ω to 3.3V.",
        "RP controls 12mA; HCLK phases ≥50us.",
    ]
    for i, note in enumerate(notes):
        s.text(1117, 893 + 28 * i, note, size=18)
    s.rect(55, 1087, 1570, 43, fill="#fff1ed", stroke="#d89987", radius=3)
    s.text(70, 1105, "Keep ALL 16 signal links OPEN until compatible Core firmware + FPGA image are installed.", size=17, weight=700)
    s.text(70, 1123, "Assembly while unpowered is OK; keep removable links open on powered old images. 100Ω does not make contention safe.", size=17, weight=600)
    s.text(55, 1148, "Core bus: P3/P4 contacts, not package pins. GP39 WREADY: RP input; shared red LED + 470Ω to GND adds Tang output load.", size=16, color=MUTED)
    s.text(55, 1170, "Sources: user wiring revision; Core2350B schematic; Tang Rev 1.30 / hardware 3921 (2023-07-11). Exact evidence: pinout JSON.", size=14.5, color=MUTED)
    return s


def tang_headers(rows, headers):
    by_header = {r["tang_header"]: r for r in rows}
    s = SVG(1188, 1680, 210, 297, "Tang Nano20K component-side 40-position J6 and J5 physical header map")
    header(s, "Tang Nano20K · physical header map", "Hardware 3921 · component side · USB-C TOP / HDMI BOTTOM")
    s.text(55, 213, "J6 = LEFT edge; J5 = RIGHT edge. Both number 1 at top → 20 at bottom.", size=20, weight=700)

    # Board is a positioning aid, not a scale drawing of traces or component locations.
    s.rect(475, 242, 240, 1110, fill="#eaf2f3", stroke="#7b9a9f", radius=20, sw=2)
    s.rect(543, 226, 104, 43, fill="#e7ebef", stroke="#6e7d8e", radius=6, sw=2)
    s.text(595, 254, "USB-C", size=17, weight=700, anchor="middle")
    s.rect(533, 1332, 124, 52, fill="#e7ebef", stroke="#6e7d8e", radius=4, sw=2)
    s.text(595, 1413, "HDMI", size=20, weight=700, anchor="middle")
    s.text(491, 287, "J6", size=22, weight=700, anchor="middle")
    s.text(699, 287, "J5", size=22, weight=700, anchor="middle")
    s.rect(483, 301, 28, 970, fill="#d0dcdd", stroke="#a0b4b8", radius=4)
    s.rect(679, 301, 28, 970, fill="#d0dcdd", stroke="#a0b4b8", radius=4)
    s.rect(540, 677, 110, 175, fill="#31545f", stroke="#183c46", radius=5)
    s.text(595, 739, "GW2AR", size=18, weight=600, color="white", anchor="middle")
    s.text(595, 766, "18", size=23, weight=600, color="white", anchor="middle")
    s.text(595, 805, "FPGA", size=18, color="white", anchor="middle")
    s.text(595, 930, "TANG", size=21, weight=700, color="#3b6872", anchor="middle")
    s.text(595, 960, "NANO 20K", size=21, weight=700, color="#3b6872", anchor="middle")
    s.text(595, 1000, "component", size=17, color="#3b6872", anchor="middle")
    s.text(595, 1025, "side", size=17, color="#3b6872", anchor="middle")

    for connector, x, pin_x, board_edge in [("J6", 55, 497, 475), ("J5", 753, 693, 715)]:
        for number in range(1, 21):
            key = f"{connector}.{number}"
            item = headers[key]
            row = by_header.get(key)
            y = 324 + (number - 1) * 48.5
            if row:
                color = color_for_signal(row["signal"])
                fill = {BLUE: "#eef5ff", TEAL: "#eaf7f4", VIOLET: "#f5effb"}[color]
                primary = f'{key} · {row["signal"]}'
                secondary = f'FPGA {row["fpga_pin"]} · GP{row["gpio"]} ({row["core_header"]})'
            elif item["type"] == "ground":
                color, fill = INK, "#edf0f4"
                primary = f"{key} · GND"
                secondary = "Connect to Core ground"
            elif item["type"] == "power":
                color, fill = POWER, "#fff4e9"
                primary = f'{key} · {item["label"]}'
                secondary = "Local Tang pull-up rail ONLY" if key == "J6.19" else "UNCONNECTED · do not join Core rail"
            else:
                color, fill = GREY, "#f4f5f7"
                primary = f'{key} · FPGA {item["fpga_pin"]}'
                secondary = item["label"].replace("initialization", "init").replace("amplifier", "amp").replace("enable", "EN").replace("audio ", "") + " · no bus wire"
            s.rect(x, y - 21, 380, 44, fill=fill, stroke=GRID, radius=3)
            s.text(x + 12, y - 2, primary, size=19, weight=700, color=color)
            s.text(x + 12, y + 17, secondary, size=16, color=color)
            s.line(x + 380 if connector == "J6" else board_edge, y, board_edge if connector == "J6" else x, y, color=color, sw=2)
            s.line(board_edge, y, pin_x, y, color=color, sw=2)
            s.circle(pin_x, y, 11.5, fill=color if row or item["type"] == "ground" else "white", stroke=color, sw=2)
            s.text(pin_x, y + 4.5, number, size=12, weight=700, color="white" if row or item["type"] == "ground" else color, anchor="middle")

    s.rect(55, 1440, 1078, 148, fill="#f8fafc", stroke=GRID, radius=4)
    s.text(73, 1466, "Blue: Core → Tang   •   Teal: Tang → Core   •   Purple: bidirectional data", size=18, weight=600)
    s.text(73, 1489, "Grey GPIOs: leave bus wiring off. Orange supply pins: never connect to a Core power rail.", size=17)
    s.text(73, 1512, "Common ground: J6.20 + J5.2 + J5.15 → Core P1.1 / P3.1. Power each board separately.", size=17)
    s.text(73, 1535, "FPC LCD disconnected. HDMI CEC/HPD (J6.10/J6.11) remain reserved for HDMI.", size=17)
    s.text(73, 1558, "Keep ALL 16 signal links OPEN until compatible Core firmware + FPGA image are installed.", size=17, weight=700)
    s.text(73, 1581, "16 × 100Ω fitted (one/wire); end placement unknown. Series R does not make contention safe.", size=17, weight=600)
    s.text(55, 1618, "Physical orientation: view the components; do not mirror this map for the solder side. Board outline is schematic, not to scale.", size=16, color=MUTED)
    s.text(55, 1648, "Source: Tang Nano20K Rev 1.30 / hardware 3921 (2023-07-11); full provenance and signal map in the pinout JSON.", size=16, color=MUTED)
    return s


def core_headers(rows):
    by_header = {r["core_header"]: r for r in rows}
    s = SVG(1188, 1680, 210, 297, "Core2350B component-side physical P4 and P3 header locator")
    header(s, "Core2350B · P4 / P3 physical locator", "RP2350B package, A2 silicon · 8 MB PSRAM / 16 MB flash · component side · USB FPC TOP")
    s.text(55, 220, "Use the Core2350B connector labels below; Pico 2 header numbering does not apply.", size=20, weight=700)
    s.text(55, 270, "P4 · RIGHT EDGE · vertical 2×8 header", size=24, weight=700)
    s.text(500, 309, "Inner / odd", size=18, weight=600, anchor="middle")
    s.text(650, 309, "Outer / even", size=18, weight=600, anchor="middle")
    s.rect(477, 329, 46, 523, fill="#eaf2f3", stroke="#9ab1b5", radius=4)
    s.rect(627, 329, 46, 523, fill="#eaf2f3", stroke="#9ab1b5", radius=4)

    preserved = {0: "Existing indicator", 2: "Existing SD SPI", 3: "Existing SD SPI", 4: "Existing SD SPI", 5: "Existing SD SPI", 6: "Existing audio I2S", 7: "Existing audio I2S", 8: "Existing audio I2S", 27: "Existing PS/2 keyboard", 28: "Existing PS/2 keyboard"}
    p4_odd = [None, 32, 34, 36, 38, 40, 42, 44]
    p4_even = [30, 31, 33, 35, 37, 39, 41, 43]

    def core_label(connector, pin, gpio):
        key = f"{connector}.{pin}"
        row = by_header.get(key)
        if row:
            note = " · Core LED load" if gpio == 39 else ""
            return color_for_signal(row["signal"]), f"{key} · GP{gpio}", f'{row["signal"]} · Tang {row["tang_header"]}{note}'
        if gpio is None:
            return INK, f"{key} · GND", "Common ground to Tang"
        role = "Unavailable for GPU bus" if gpio in range(10, 16) else preserved.get(gpio, "Unallocated by this bus layout")
        return GREY, f"{key} · GP{gpio}", role

    for index in range(8):
        y = 361 + index * 66
        for pin, gpio, label_x, pin_x in [(1 + 2 * index, p4_odd[index], 55, 500), (2 + 2 * index, p4_even[index], 706, 650)]:
            color, primary, secondary = core_label("P4", pin, gpio)
            active = f"P4.{pin}" in by_header or gpio is None
            s.rect(label_x, y - 25, 427 if pin % 2 == 0 else 387, 51, fill="#f3f6f9" if active else "#f6f7f8", stroke=GRID, radius=3)
            s.text(label_x + 13, y - 4, primary, size=21, weight=700, color=color)
            s.text(label_x + 13, y + 17, secondary, size=16 if gpio == 39 else 18, color=color)
            s.line(label_x + 387 if pin % 2 else pin_x, y, pin_x if pin % 2 else label_x, y, color=color, sw=2)
            s.circle(pin_x, y, 15, fill=color if active else "white", stroke=color, sw=2)
            s.text(pin_x, y + 5, pin, size=15, weight=700, color="white" if active else color, anchor="middle")

    s.text(55, 886, "P4 contacts read TOP → BOTTOM. Outer column is toward the board's right edge.", size=19, color=MUTED)
    s.text(55, 915, "GP39: WREADY input only; red LED + 470Ω to GND loads the Tang output. Disable RP LED writes.", size=18, color=POWER)
    s.text(55, 969, "P3 · BOTTOM EDGE · horizontal 2×8 header", size=24, weight=700)
    s.text(55, 1002, "Read both rows LEFT → RIGHT. Inner / even row is nearer the board centre.", size=19, color=MUTED)
    s.rect(91, 1100, 1038, 139, fill="#eaf2f3", stroke="#9ab1b5", radius=4)
    p3_even = [15, 16, 18, 20, 22, 24, 26, 28]
    p3_odd = [None, 17, 19, 21, 23, 25, 27, 29]
    for index in range(8):
        x = 148 + index * 132
        for pin, gpio, y, label_y in [(2 + 2 * index, p3_even[index], 1125, 1038), (1 + 2 * index, p3_odd[index], 1214, 1268)]:
            color, _, secondary = core_label("P3", pin, gpio)
            active = f"P3.{pin}" in by_header or gpio is None
            s.circle(x, y, 18, fill=color if active else "white", stroke=color, sw=2)
            s.text(x, y + 6, pin, size=18, weight=700, color="white" if active else color, anchor="middle")
            s.text(x, label_y, f"P3.{pin}", size=18, weight=600, color=color, anchor="middle")
            s.text(x, label_y + 25, "GND" if gpio is None else f"GP{gpio}", size=21, weight=700, color=color, anchor="middle")
            row = by_header.get(f"P3.{pin}")
            short_label = row["signal"] if row else "Common GND" if gpio is None else "PS/2 kept" if gpio in (27, 28) else "Unavailable" if gpio in range(10, 16) else "Unallocated"
            s.text(x, label_y + 49, short_label, size=16, color=color, anchor="middle")
    s.text(95, 1169, "INNER / EVEN", size=16, weight=600, color="#3b6872")
    s.text(915, 1169, "OUTER / ODD", size=16, weight=600, color="#3b6872")

    s.rect(55, 1370, 1078, 223, fill="#f8fafc", stroke=GRID, radius=4)
    s.text(73, 1401, "Physical orientation reference", size=21, weight=700)
    s.text(73, 1433, "Component side, USB FPC toward top:", size=19)
    s.text(73, 1462, "P1 left · P2 top · P3 bottom · P4 right.", size=19)
    s.text(73, 1490, "Signals use P3/P4. Bond P1.1 GND to Tang J5.2 as well.", size=17)
    s.text(73, 1521, "Keep ALL 16 signal links OPEN until compatible Core firmware", size=17, weight=700)
    s.text(73, 1545, "+ FPGA image are installed; old powered images need links open.", size=17, weight=700)
    s.text(73, 1569, "One 100Ω per signal wire; end unknown. R does not make contention safe.", size=17, weight=600)
    s.rect(918, 1424, 141, 117, fill="#eaf2f3", stroke="#7b9a9f", radius=5)
    s.rect(966, 1415, 46, 16, fill="#b8c8cc", stroke="#7b9a9f", radius=2)
    s.text(989, 1405, "USB FPC ↑", size=16, weight=600, anchor="middle")
    s.rect(910, 1440, 13, 73, fill="#31545f", stroke="none")
    s.rect(1054, 1440, 13, 73, fill="#31545f", stroke="none")
    s.rect(948, 1422, 80, 10, fill="#31545f", stroke="none")
    s.rect(948, 1534, 80, 10, fill="#31545f", stroke="none")
    s.text(899, 1483, "P1", size=18, weight=700, anchor="end")
    s.text(1080, 1483, "P4", size=18, weight=700)
    s.text(989, 1454, "P2", size=16, weight=700, anchor="middle")
    s.text(989, 1573, "P3", size=18, weight=700, anchor="middle")
    s.text(55, 1620, "Connector locator only. P3/P4 drawings are expanded for legibility and are not a scale board drawing. Do not mirror for solder side.", size=15, color=MUTED)
    s.text(55, 1648, "Sources: user wiring revision, Core2350B schematic and component-side photograph; provenance in the pinout JSON.", size=16, color=MUTED)
    return s


def load_pinout():
    """The JSON-to-display normalization is intentionally kept in one place."""
    data = json.loads(PINOUT.read_text(encoding="utf-8"))
    if data["rp_board"]["silicon_stepping"] != "A2":
        raise ValueError("Update the page headings for changed RP2350 silicon")
    if data["fixed_bus_test_configuration"]["core_sdram_hz"] != 6300000 or data["fixed_bus_test_configuration"]["host_min_phase_and_setup_us"] != 50:
        raise ValueError("Update the bus-test clock notes for changed core/host timing")
    if data["assembly"]["parts"] != {"100_ohm_series_resistors": 16, "4700_ohm_handshake_pulldowns": 2}:
        raise ValueError("Update the confirmed assembly notes for changed fitted parts")
    observed_bias = data["assembly"]["observed_external_bias"]
    if observed_bias["WREADY_pull_down_fitted"] is not True or observed_bias["RVALID_pull_down_fitted"] is not True:
        raise ValueError("Update the page warnings for changed handshake pull-downs")
    expected_pulls = {
        "up": {"WR_N", "RD_N", "ALE_N"},
        "down": {"HCLK", "ABORT", "GPU_RESET_N", "WREADY", "RVALID"},
    }
    for direction, expected in expected_pulls.items():
        actual = {item["signal"] for item in data["signals"] if item["pull"]["direction"] == direction}
        if actual != expected:
            raise ValueError("Update the printed bias notes for changed pull assignments")
    directions = {"rp_to_gpu": "core_to_tang", "gpu_to_rp": "tang_to_core", "bidirectional": "bidirectional"}
    rows = []
    for item in data["signals"]:
        if item["io_voltage_v"] != 3.3 or item["series_resistors"]["ohms"] != 100:
            raise ValueError("Update drawing labels for changed I/O voltage or series resistance")
        if item["pull"]["direction"] != "none" and item["pull"]["ohms"] != 4700:
            raise ValueError("Update drawing labels for changed local pull resistance")
        if item["series_resistors"]["count_per_wire"] != 1 or item["series_resistors"]["locations"] != ["not reported"]:
            raise ValueError("Update the drawing for changed resistor count or known placement")
        rows.append({
            "signal": item["signal"], "gpio": item["rp_gpio"],
            "core_header": f'{item["rp_header"]}.{item["rp_header_pin"]}',
            "tang_header": f'{item["tang_header"]}.{item["tang_header_pin"]}',
            "fpga_pin": item["fpga_package_pin"], "bank": item["fpga_bank"],
            "direction": directions[item["direction"]],
        })
    headers = {}
    for connector, positions in data["tang_headers"].items():
        for item in positions:
            label = item["onboard_function"]
            kind = "ground" if label == "GND" else "power" if label in ("3.3V", "5V") else "gpio"
            headers[f'{connector}.{item["pin"]}'] = {
                "type": kind, "label": label, "fpga_pin": item["fpga_package_pin"],
            }
    if len(rows) != 16 or len(headers) != 40:
        raise ValueError("These page layouts require 16 signals and 40 Tang header positions")
    # The physical connector routing comes from the manufacturer schematic.
    # Reject a changed bus JSON that contradicts that fixed connector routing.
    physical_core = {}
    for connector, odd, even in [
        ("P1", [None, 2, 4, 6, 8, 10, 12, 14], [0, 1, 3, 5, 7, 9, 11, 13]),
        ("P3", [None, 17, 19, 21, 23, 25, 27, 29], [15, 16, 18, 20, 22, 24, 26, 28]),
        ("P4", [None, 32, 34, 36, 38, 40, 42, 44], [30, 31, 33, 35, 37, 39, 41, 43]),
    ]:
        for index, gpio in enumerate(odd):
            physical_core[f"{connector}.{index * 2 + 1}"] = gpio
        for index, gpio in enumerate(even):
            physical_core[f"{connector}.{index * 2 + 2}"] = gpio
    for row in rows:
        if physical_core.get(row["core_header"]) != row["gpio"]:
            raise ValueError("Bus JSON contradicts verified Core2350B connector routing")
        if headers[row["tang_header"]]["fpga_pin"] != row["fpga_pin"]:
            raise ValueError("Bus JSON and physical Tang header table disagree")
    return rows, headers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--svg-only", action="store_true", help="Skip Inkscape PDF export")
    args = parser.parse_args()
    rows, headers = load_pinout()
    IMAGES.mkdir(parents=True, exist_ok=True)
    drawings = [
        ("core2350b-tang-nano20k-wiring", wiring(rows)),
        ("tang-nano20k-header-map", tang_headers(rows, headers)),
        ("core2350b-header-map", core_headers(rows)),
    ]
    with tempfile.TemporaryDirectory(prefix="n20k-wiring-") as tmp:
        render_env = os.environ.copy()
        render_env["INKSCAPE_PROFILE_DIR"] = str(Path(tmp) / "inkscape")
        render_env["XDG_CACHE_HOME"] = str(Path(tmp) / "cache")
        pdf_paths = []
        for basename, drawing in drawings:
            svg_path = IMAGES / f"{basename}.svg"
            drawing.save(svg_path)
            print(svg_path.relative_to(ROOT))
            if not args.svg_only:
                if not shutil.which("inkscape") or not shutil.which("pdfunite"):
                    raise SystemExit("Inkscape and pdfunite are required for PDF export; use --svg-only to skip it.")
                pdf_path = Path(tmp) / f"{basename}.pdf"
                subprocess.run(["inkscape", str(svg_path), "--export-type=pdf", f"--export-filename={pdf_path}"], check=True, env=render_env)
                pdf_paths.append(pdf_path)
        if not args.svg_only:
            # Mixed A4 landscape/portrait pages preserve the intended home-print sizes.
            bundle = IMAGES / "core2350b-tang-nano20k-wiring.pdf"
            subprocess.run(["pdfunite", *(str(p) for p in pdf_paths), str(bundle)], check=True)
            print(f"{bundle.relative_to(ROOT)} (3-page A4 bundle)")


if __name__ == "__main__":
    main()
