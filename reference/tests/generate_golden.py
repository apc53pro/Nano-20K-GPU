#!/usr/bin/env python3
"""Translate the specification's JSON vectors into C++ conformance checks."""

import argparse
from fractions import Fraction
import json
from pathlib import Path


def point(value):
    return "Point{" + ", ".join(str(int(v)) for v in value) + "}"


def point_list(values):
    return "std::vector<Point>{" + ", ".join(point(p) for p in values) + "}"


def positions(values):
    return "std::array<Point, 3>{{" + ", ".join(point(p) for p in values) + "}}"


def integer(value):
    return str(int(value)) + "LL"


def plane(value):
    return "Plane{" + ", ".join(integer(value[k]) for k in ("seed", "step_x", "step_y")) + "}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vectors", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = json.loads(args.vectors.read_text())
    lines = [
        "// Generated from docs/spec/golden-vectors.json. Do not edit.",
        "#pragma once",
        "void golden_tests() {",
        '    run_case("golden profile and tile metadata", [] {',
        f'        expect(std::string("{data["profile"]}") == "N20K-M0-0.1", "golden profile");',
        f'        expect(tile_size == {data["tile_width"]} && tile_size == {data["tile_height"]}, "golden tile dimensions");',
        "    });",
    ]
    for case in data["coverage"]:
        lines.extend([
            f'    run_case("golden {case["id"]}", [] {{',
            f'        const auto t = screen_triangle({positions(case["vertices_pixels"])});',
            f'        expect(point_set(coverage_points(t)) == point_set({point_list(case["expected_pixels"])}), "coverage vector");',
            "    });",
        ])
    case = data["tile_crossing"]
    lines.extend([
        f'    run_case("golden {case["id"]}", [] {{',
        "        std::vector<Primitive> primitives;",
    ])
    for triangle in case["triangles_pixels"]:
        lines.append(f"        primitives.emplace_back(screen_triangle({positions(triangle)}));")
    lines.extend([
        "        const auto bins = bin_primitives(primitives);",
        "        std::vector<int> active;",
        "        for (int tile = 0; tile < tile_count; ++tile) if (!bins[tile].empty()) active.push_back(tile);",
        "        const std::vector<int> expected_tiles{" + ", ".join(map(str, case["expected_tile_ids"])) + "};",
        '        expect(active == expected_tiles, "tile reference identities");',
        "        std::map<std::pair<int, int>, int> owners;",
        "        for (const auto& primitive : primitives) for (auto p : coverage_points(std::get<Triangle>(primitive))) ++owners[{p.x,p.y}];",
        f'        expect(owners.size() == {len(case["expected_pixels"])}, "tile coverage count");',
    ])
    for p in case["expected_pixels"]:
        lines.append(f'        expect(owners[{{{p[0]}, {p[1]}}}] == 1, "tile pixel has exactly one owner");')
    lines.extend(["    });"])
    for case in data["depth"]:
        if case["compare"] != "LESS":
            raise ValueError("Generator must be extended for a new golden depth function")
        lines.extend([
            f'    run_case("golden {case["id"]}", [] {{',
            "        Renderer renderer;",
        ])
        for index, (incoming, accepted) in enumerate(zip(case["incoming"], case["expected_pass"])):
            lines.extend([
                "        {",
                "            Pass pass;",
                f'            pass.color_load = LoadOp::{"Clear" if index == 0 else "Load"};',
                f'            pass.depth_load = LoadOp::{"Clear" if index == 0 else "Load"};',
                f'            pass.clear_depth = {case["clear"]}u;',
                f"            pass.primitives.emplace_back(rectangle(Bounds{{1,1,2,2}}, Color{{255,255,255}}, {incoming}u, true));",
                "            renderer.render(pass);",
                f'            expect(renderer.statistics().color_writes == {int(accepted)}u, "depth pass/reject sequence");',
                "        }",
            ])
        lines.extend([
            f'        expect(renderer.depth().at(pixel_index(1,1)) == {case["expected_final"]}u, "final D24 value");',
            "    });",
        ])
    case = data["color_key"]
    lines.extend([
        f'    run_case("golden {case["id"]}", [] {{',
        "        Renderer renderer; Pass pass;",
        f'        pass.clear_color = {case["clear_color_rgb565"]}u;',
        f'        pass.clear_depth = {case["clear_depth"]}u;',
    ])
    for fragment in case["fragments"]:
        lines.extend([
            "        {",
            "            Sprite sprite; sprite.bounds = Bounds{0,0,1,1};",
            f'            sprite.state.texture = solid_texture({fragment["texel_rgb565"]}u);',
            "            sprite.state.depth_test = true; sprite.state.depth_write = true;",
            f'            sprite.depth = {fragment["depth"]}u;',
        ])
        if fragment["key_enabled"]:
            lines.append(f'            sprite.state.color_key = {fragment["key_rgb565"]}u;')
        lines.extend(["            pass.primitives.emplace_back(sprite);", "        }"])
    lines.extend([
        "        renderer.render(pass);",
        f'        expect(renderer.depth().at(0) == {case["expected_final_depth"]}u, "key rejection must not commit depth");',
        f'        expect(renderer.color(1).at(0) == {case["expected_final_color_rgb565"]}u, "farther opaque texel survives");',
        "    });",
    ])
    case = data["perspective"]
    q = [Fraction(*pair) for pair in case["q_fraction_pairs"]]
    weights = [Fraction(*pair) for pair in case["expected_barycentric_pairs"]]
    interpolated_q = sum(a * b for a, b in zip(q, weights))
    ideal_u = sum(w * u * reciprocal for w, u, reciprocal in zip(weights, case["u_texels"], q)) / interpolated_q
    ideal_v = sum(w * v * reciprocal for w, v, reciprocal in zip(weights, case["v_texels"], q)) / interpolated_q
    if ideal_u != Fraction(*case["expected_u_fraction"]) or ideal_v != Fraction(*case["expected_v_fraction"]):
        raise ValueError("Perspective rational vector is internally inconsistent")
    affine = [sum(w * coordinate for w, coordinate in zip(weights, case[axis])) for axis in ("u_texels", "v_texels")]
    if [value.numerator // value.denominator for value in affine] != case["affine_texel"]:
        raise ValueError("Affine contrast vector is internally inconsistent")
    lines.extend([
        f'    run_case("golden {case["id"]}", [] {{',
        "        std::array<ScreenVertex,3> vertices{};",
    ])
    for i in range(3):
        x, y = case["vertices_pixels"][i]
        numerator, denominator = case["q_fraction_pairs"][i]
        lines.extend([
            f"        vertices[{i}].x = {x}; vertices[{i}].y = {y}; vertices[{i}].depth = 0.5L;",
            f"        vertices[{i}].w = static_cast<long double>({denominator}) / {numerator};",
            f'        vertices[{i}].u = {case["u_texels"][i]}; vertices[{i}].v = {case["v_texels"][i]};',
        ])
    lines.extend([
        "        RenderState state; state.shade = ShadeMode::TextureReplace; state.texture = solid_texture(0xffffu,8,8);",
        "        const auto triangle = require_triangle(vertices, state);",
        f'        expect(triangle.bounds.x0 == {case["anchor_pixel"][0]} && triangle.bounds.y0 == {case["anchor_pixel"][1]}, "UV anchor");',
    ])
    for member, key in (("q", "q_plane_q37"), ("u", "U_plane_q27"), ("v", "V_plane_q27")):
        lines.append(f'        expect(same_plane(triangle.{member}, {plane(case[key])}), "{key}");')
    lines.extend([
        f'        const auto uv = texture_coordinates(triangle, {case["sample_pixel"][0]}, {case["sample_pixel"][1]});',
        f'        expect(uv == {point(case["canonical_uv_q16"])}, "canonical Q16 perspective coordinates");',
        f'        expect(Point{{floor_q16(uv.x),floor_q16(uv.y)}} == {point(case["expected_texel"])}, "perspective texel selection");',
        f'        expect(Point{{floor_q16(uv.x),floor_q16(uv.y)}} != {point(case["affine_texel"])}, "perspective differs from affine");',
        "    });",
    ])
    case = data["line"]
    lines.extend([
        f'    run_case("golden {case["id"]}", [] {{',
        f'        const auto expected = {point_list(case["expected_pixels"])};',
        f'        expect(line_pixels({point(case["endpoints"][0])}, {point(case["endpoints"][1])}) == expected, "global line sequence");',
        f'        expect(line_pixels({point(case["endpoints"][1])}, {point(case["endpoints"][0])}) == expected, "line reversal invariance");',
        "    });",
    ])
    case = data["color"]
    channels = ",".join(map(str, case["modulation_rgb888"]))
    lines.extend([
        f'    run_case("golden {case["id"]}", [] {{',
        f'        const auto result = pack_rgb565(modulate(expand_rgb565({case["texel_rgb565"]}u), Color{{{channels}}}));',
        f'        expect(result == {case["expected_rgb565"]}u, "RGB565 modulation vector");',
        "        for (std::uint32_t value=0; value<=0xffffu; ++value) {",
        "            const auto expanded = expand_rgb565(static_cast<std::uint16_t>(value));",
        '            expect(pack_rgb565(expanded) == value, "RGB565 expansion/packing identity");',
        '            expect(pack_rgb565(modulate(expanded,Color{255,255,255})) == value, "white modulation identity");',
        "        }",
        "    });",
        "}",
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
