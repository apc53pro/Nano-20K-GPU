#include "nano_gpu/hw/bsram.hpp"
#include "nano_gpu/hw/device.hpp"
#include "nano_gpu/hw/mult18x18.hpp"
#include "nano_gpu/hw/tile_pipeline.hpp"
#include "nano_gpu/reference.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace nano_gpu;
using namespace nano_gpu::hw;

namespace {

int cases_run = 0;
int cases_failed = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void expect_throws(Function&& function, const std::string& message) {
    bool threw = false;
    try { function(); }
    catch (const std::exception&) { threw = true; }
    expect(threw, message);
}

template<class Function>
void run_case(const std::string& name, Function&& function) {
    ++cases_run;
    try { function(); }
    catch (const std::exception& error) {
        ++cases_failed;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

void multiplier_tests() {
    run_case("MULT18X18 starts unknown and has three enabled-edge latency", [] {
        Mult18x18 multiplier;
        expect(!multiplier.output_known(), "power-up output is not promised zero");
        multiplier.tick({11, -7, true, false});
        expect(!multiplier.output_known(), "operand capture is not a completed multiply");
        multiplier.tick({3, 9, true, false});
        expect(!multiplier.output_known(), "product register is not the output register");
        multiplier.tick({-2, 17, true, false});
        expect(multiplier.output_known() && multiplier.product() == -77,
               "first product appears on the third enabled edge");
        multiplier.tick({0, 0, true, false});
        expect(multiplier.product() == 27, "successive products retain order");
        multiplier.tick({0, 0, true, false});
        expect(multiplier.product() == -34, "signed result reaches the output");
    });

    run_case("MULT18X18 preserves the full signed 36-bit product", [] {
        constexpr std::int32_t low = -131072;
        constexpr std::int32_t high = 131071;
        const std::vector<std::pair<std::int32_t, std::int32_t>> operands{
            {low, low}, {high, high}, {low, high}, {-1, high}, {high, -1},
            {-1, -1}, {0, low}, {1, high}, {0, 0}, {0, 0}};
        const std::array<std::int64_t, 8> expected{{
            17179869184LL, 17179607041LL, -17179738112LL, -131071,
            -131071, 1, 0, 131071}};
        Mult18x18 multiplier;
        for (std::size_t i = 0; i < operands.size(); ++i) {
            multiplier.tick({operands[i].first, operands[i].second, true, false});
            if (i >= 2) {
                expect(multiplier.output_known() && multiplier.product() == expected[i - 2],
                       "extreme signed product must not truncate to a host 32-bit value");
            }
        }
    });

    run_case("MULT18X18 CE stalls the entire pipeline", [] {
        Mult18x18 multiplier;
        std::vector<std::int64_t> products;
        std::mt19937 random(0x18c0ffeeu);
        for (int cycle = 0; cycle < 97; ++cycle) {
            const bool enabled = cycle % 4 != 1 && cycle % 7 != 2;
            const auto a = static_cast<std::int32_t>(random() % 262144u) - 131072;
            const auto b = static_cast<std::int32_t>(random() % 262144u) - 131072;
            const bool known_before = multiplier.output_known();
            const auto value_before = multiplier.product();
            if (enabled) {
                multiplier.tick({a, b, true, false});
                products.push_back(static_cast<std::int64_t>(a) * b);
                if (products.size() >= 3) {
                    expect(multiplier.output_known(), "three captured products make output known");
                    expect(multiplier.product() == products[products.size() - 3],
                           "stalls must preserve accepted-product order");
                }
            } else {
                multiplier.tick({std::numeric_limits<std::int32_t>::max(),
                                 std::numeric_limits<std::int32_t>::min(), false, false});
                expect(multiplier.output_known() == known_before && multiplier.product() == value_before,
                       "CE-low holds output and does not capture out-of-width inputs");
            }
        }
    });

    run_case("MULT18X18 synchronous reset takes priority over CE", [] {
        Mult18x18 multiplier;
        multiplier.tick({91, -13, true, false});
        multiplier.tick({7, 23, true, false});
        multiplier.tick({5, 19, true, false});
        multiplier.tick({std::numeric_limits<std::int32_t>::max(),
                         std::numeric_limits<std::int32_t>::min(), false, true});
        expect(multiplier.output_known() && multiplier.product() == 0,
               "reset clears registers even when CE is low");
        multiplier.tick({13, 7, true, false});
        expect(multiplier.product() == 0, "old operands must not survive reset");
        multiplier.tick({13, 7, true, false});
        expect(multiplier.product() == 0, "old intermediate product must not survive reset");
        multiplier.tick({13, 7, true, false});
        expect(multiplier.product() == 91, "new work completes normally after reset");
    });

    run_case("MULT18X18 rejects illegal captured widths without advancing", [] {
        Mult18x18 actual;
        actual.tick({-37, 41, true, false});
        actual.tick({43, 47, true, false});
        const Mult18x18 before = actual;
        expect_throws([&] { actual.tick({131072, 1, true, false}); }, "positive 19-bit A rejected");
        expect_throws([&] { actual.tick({1, -131073, true, false}); }, "negative 19-bit B rejected");
        Mult18x18 expected = before;
        for (int i = 0; i < 8; ++i) {
            const Mult18Cycle request{i * 3, -i * 5, true, false};
            actual.tick(request);
            expected.tick(request);
            expect(actual.output_known() == expected.output_known() &&
                   actual.product() == expected.product(), "invalid requests must not change any stage");
        }
    });
}

void bsram_tests() {
    run_case("SDPB initial output is unknown and configured RAM INIT is zero", [] {
        Sdp1kx16 ram;
        expect(!ram.output_known(), "output latch requires a clocked read or reset");
        expect_throws([&] { (void)ram.output(); }, "unknown output cannot be observed as valid data");
        for (std::uint16_t address = 0; address < Sdp1kx16::words; ++address) {
            BsramCycle cycle;
            cycle.read_enable = true;
            cycle.read_address = address;
            ram.tick(cycle);
            expect(ram.output_known() && ram.output() == 0, "explicit zero INIT applies at every address");
        }
    });

    run_case("SDPB synchronous reads take one edge and hold with CE low", [] {
        Sdp1kx16 ram;
        ram.tick({true, 17, 0xa55au, false, 0, false});
        expect(!ram.output_known(), "a write does not update the read latch");
        ram.tick({false, 0, 0, true, 17, false});
        expect(ram.output() == 0xa55au, "one read edge captures the selected word");
        ram.tick({false, 65535, 0, false, 65535, false});
        expect(ram.output() == 0xa55au, "inactive ports ignore addresses and hold output");
        ram.tick({true, 23, 0x5aa5u, true, 17, false});
        expect(ram.output() == 0xa55au, "different-address read and write can share an edge");
        ram.tick({false, 0, 0, true, 23, false});
        expect(ram.output() == 0x5aa5u, "write from the previous edge is visible");
    });

    run_case("SDPB forbidden read-write collisions do not invent a RAM mode", [] {
        Sdp1kx16 ram;
        ram.tick({true, 42, 0x1357u, false, 0, false});
        ram.tick({false, 0, 0, true, 42, false});
        expect_throws([&] { ram.tick({true, 42, 0x2468u, true, 42, false}); },
                      "same-address read and write is prohibited by the selected primitive");
        expect(ram.output() == 0x1357u, "collision must not change the output latch");
        ram.tick({false, 0, 0, true, 42, false});
        expect(ram.output() == 0x1357u, "collision must not partially perform its write");
    });

    run_case("SDPB output reset preserves stored words", [] {
        Sdp1kx16 ram;
        ram.tick({true, 1023, 0xffffu, false, 0, false});
        ram.tick({false, 0, 0, true, 1023, false});
        expect(ram.output() == 0xffffu, "last physical word is reachable");
        ram.tick({false, 0, 0, false, 0, true});
        expect(ram.output_known() && ram.output() == 0, "reset zeros the output with no read request");
        ram.tick({false, 0, 0, true, 1023, false});
        expect(ram.output() == 0xffffu, "reset must not act as a free RAM clear");
        expect_throws([&] { ram.tick({true, 1023, 0, false, 0, true}); },
                      "writes while reset is asserted are outside the chosen configuration");
        expect(ram.output() == 0xffffu, "illegal reset/write must not reset or write partially");
        ram.tick({false, 0, 0, true, 1023, false});
        expect(ram.output() == 0xffffu, "reset/write rejection preserves contents");
    });

    run_case("SDPB checks enabled addresses before changing either port", [] {
        Sdp1kx16 ram;
        ram.tick({true, 2, 0xbabeu, false, 0, false});
        ram.tick({false, 0, 0, true, 2, false});
        expect_throws([&] { ram.tick({true, 1024, 0, true, 0, false}); },
                      "write address wider than ten bits rejected");
        expect(ram.output() == 0xbabeu, "invalid write must not perform the otherwise valid read");
        expect_throws([&] { ram.tick({true, 2, 0, true, 1024, false}); },
                      "read address wider than ten bits rejected");
        ram.tick({false, 0, 0, true, 2, false});
        expect(ram.output() == 0xbabeu, "invalid read must not perform the otherwise valid write");
    });

    run_case("SDPB full storage and native address packing", [] {
        Sdp1kx16 ram;
        for (std::uint16_t address = 0; address < Sdp1kx16::words; ++address) {
            ram.tick({true, address, static_cast<std::uint16_t>((address * 73u) ^ 0x55aau), false, 0, false});
        }
        for (std::uint16_t address = 0; address < Sdp1kx16::words; ++address) {
            ram.tick({false, 0, 0, true, address, false});
            expect(ram.output() == static_cast<std::uint16_t>((address * 73u) ^ 0x55aau),
                   "every address must store a distinct full-width word");
            expect(Sdp1kx16::write_bus_address(address) == static_cast<std::uint16_t>(address * 16u + 3u),
                   "write address must enable both bytes in ADA low bits");
            expect(Sdp1kx16::read_bus_address(address) == static_cast<std::uint16_t>(address * 16u),
                   "read address uses the native AD[13:4] word selection");
        }
        expect(Sdp1kx16::write_bus_address(1023) == 0x3ff3u &&
               Sdp1kx16::read_bus_address(1023) == 0x3ff0u, "address endpoints match documented packing");
        expect_throws([] { (void)Sdp1kx16::write_bus_address(1024); }, "write bus helper rejects aliasing");
        expect_throws([] { (void)Sdp1kx16::read_bus_address(65535); }, "read bus helper rejects aliasing");
    });
}

void resource_tests() {
    run_case("device capacity distinguishes native resources from selected tile banks", [] {
        expect(TangNano20K::bsram_blocks == 46 && TangNano20K::mult18_equivalents == 48,
               "capacity must use the selected QN88 device resource inventory");
        expect(TangNano20K::dsp_blocks == 12 && TangNano20K::dsp_macros == 24,
               "18x18 equivalents must not be mistaken for physical DSP blocks");
        expect(TangNano20K::bsram_bits_per_block == 18432 && Sdp1kx16::words * Sdp1kx16::word_bits == 16384,
               "chosen 16-bit mode does not expose the physical parity-bit capacity as free data");
        expect(OpaqueSliceResources::bsram_blocks == 3 && OpaqueSliceResources::mult18_equivalents == 1,
               "one resident RGB565/D24 tile requires three banks and the chosen shared multiplier");
        check_resource_capacity(TangNano20K::bsram_blocks, TangNano20K::mult18_equivalents);
        check_resource_capacity(OpaqueSliceResources::bsram_blocks, OpaqueSliceResources::mult18_equivalents);
        expect_throws([] { check_resource_capacity(47, 1); }, "BSRAM capacity cannot silently overcommit");
        expect_throws([] { check_resource_capacity(3, 49); }, "DSP capacity cannot silently overcommit");
        expect_throws([] { check_resource_capacity(16 * OpaqueSliceResources::bsram_blocks, 16); },
                      "sixteen such resident tiles already exceed the entire BSRAM inventory");
    });
}

Triangle triangle(std::array<ScreenVertex, 3> vertices, const RenderState& state = {}) {
    auto result = setup_triangle(vertices, state);
    if (!result) throw std::runtime_error("test triangle unexpectedly culled");
    return *result;
}

Triangle flat_triangle(std::array<Point, 3> positions, Color color, std::uint32_t depth,
                       RenderState state = {}) {
    state.color = color;
    std::array<ScreenVertex, 3> vertices{};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        vertices[i].x = positions[i].x;
        vertices[i].y = positions[i].y;
        vertices[i].depth = static_cast<long double>(depth) / depth_max;
    }
    return triangle(vertices, state);
}

void square(Pass& pass, Bounds area, Color color, std::uint32_t depth, RenderState state = {}) {
    pass.primitives.emplace_back(flat_triangle({{{area.x0, area.y0}, {area.x1, area.y0},
                                                {area.x0, area.y1}}}, color, depth, state));
    pass.primitives.emplace_back(flat_triangle({{{area.x1, area.y0}, {area.x1, area.y1},
                                                {area.x0, area.y1}}}, color, depth, state));
}

std::size_t pixel_index(int x, int y) {
    return static_cast<std::size_t>(y) * screen_width + x;
}

void compare_renderers(const Renderer& expected, const Renderer& actual) {
    expect(actual.color(0) == expected.color(0), "physical color buffer zero differs from canonical SRR");
    expect(actual.color(1) == expected.color(1), "physical color buffer one differs from canonical SRR");
    expect(actual.depth() == expected.depth(), "D24 attachment differs from canonical SRR");
    const auto& a = actual.statistics();
    const auto& e = expected.statistics();
    expect(a.candidates == e.candidates, "candidate positions differ");
    expect(a.covered_fragments == e.covered_fragments, "coverage count differs");
    expect(a.depth_rejected == e.depth_rejected, "depth rejection count differs");
    expect(a.color_writes == e.color_writes && a.depth_writes == e.depth_writes,
           "committed attachment-write counts differ");
    expect(a.tile_references == e.tile_references && a.tiles_processed == tile_count,
           "ordered bin references and complete tile traversal differ");
    expect(a.hardware_cycles > 0, "hardware block mode must report clocked work");
    expect(a.texture_samples == 0 && a.keyed_rejected == 0, "opaque slice cannot claim texture work");
}

void compare_pass(const Pass& pass) {
    Renderer expected;
    Renderer actual;
    expected.render(pass, RenderMode::Tiled);
    actual.render(pass, RenderMode::HardwareBlocks);
    compare_renderers(expected, actual);
}

void renderer_tests() {
    run_case("block adapter clears all tiles including empty bins", [] {
        Pass pass;
        pass.clear_color = 0x07e0;
        pass.clear_depth = 0x654321;
        compare_pass(pass);
        Renderer actual;
        actual.render(pass, RenderMode::HardwareBlocks);
        for (std::size_t i = 0; i < actual.depth().size(); ++i) {
            expect(actual.color(1)[i] == 0x07e0 && actual.depth()[i] == 0x654321,
                   "empty bins must not skip attachment clear");
        }
    });

    run_case("block raster crosses all tile rows and columns", [] {
        Pass pass;
        square(pass, Bounds{0, 0, screen_width, screen_height}, Color{37, 113, 229}, 0xabcdef);
        compare_pass(pass);
    });

    run_case("block raster shared edges and all Q4 offsets", [] {
        Pass pass;
        for (int subpixel = 0; subpixel < 16; ++subpixel) {
            const long double x = 15 + 34 * (subpixel % 8) + subpixel / 16.0L;
            const long double y = 15 + 67 * (subpixel / 8) + subpixel / 16.0L;
            const std::array<ScreenVertex, 4> vertices{{
                {x, y, 0.25L}, {x + 33, y, 0.25L},
                {x + 33, y + 63, 0.25L}, {x, y + 63, 0.25L}}};
            RenderState state;
            state.color = Color{static_cast<std::uint8_t>(16 * subpixel), 191, 73};
            pass.primitives.emplace_back(triangle({vertices[0], vertices[1], vertices[3]}, state));
            pass.primitives.emplace_back(triangle({vertices[3], vertices[2], vertices[1]}, state));
        }
        compare_pass(pass);
    });

    run_case("block interpolation handles clipped Gouraud and nonconstant D24", [] {
        Pass pass;
        square(pass, Bounds{0, 0, 211, 181}, Color{17, 29, 41}, 0xeeeeee);
        RenderState state;
        state.scissor = Bounds{31, 29, 173, 139};
        state.shade = ShadeMode::Gouraud;
        std::array<ScreenVertex, 3> vertices{{{2.125L, 5.375L, 0.1L},
                                             {203.75L, 31.125L, 0.8L},
                                             {41.875L, 177.625L, 0.4L}}};
        vertices[0].r = 255; vertices[0].g = 0; vertices[0].b = 32;
        vertices[1].r = 7; vertices[1].g = 255; vertices[1].b = 0;
        vertices[2].r = 0; vertices[2].g = 16; vertices[2].b = 255;
        pass.primitives.emplace_back(triangle(vertices, state));
        compare_pass(pass);
    });

    run_case("block depth distinguishes low D24 bits and equality", [] {
        Pass pass;
        square(pass, Bounds{63, 63, 64, 64}, Color{255, 0, 0}, 0x800001);
        square(pass, Bounds{63, 63, 64, 64}, Color{0, 255, 0}, 0x800000);
        square(pass, Bounds{63, 63, 64, 64}, Color{0, 0, 255}, 0x800000);
        compare_pass(pass);
        Renderer actual;
        actual.render(pass, RenderMode::HardwareBlocks);
        expect(actual.depth()[pixel_index(63, 63)] == 0x800000,
               "depth path must preserve meaningful bits below D16");
        expect(actual.color(1)[pixel_index(63, 63)] == 0x07e0,
               "nearer green passes and equal blue fails LESS");
    });

    run_case("block depth comparison and independent attachment enables", [] {
        Pass pass;
        square(pass, Bounds{10, 10, 80, 70}, Color{255, 0, 0}, 0x600000);
        RenderState lequal;
        lequal.depth_func = DepthFunc::LessEqual;
        square(pass, Bounds{20, 20, 70, 60}, Color{0, 255, 0}, 0x600000, lequal);
        RenderState always;
        always.depth_func = DepthFunc::Always;
        always.depth_write = false;
        square(pass, Bounds{30, 30, 60, 50}, Color{0, 0, 255}, 0x900000, always);
        RenderState depth_only;
        depth_only.color_write = false;
        square(pass, Bounds{25, 25, 65, 55}, Color{255, 255, 255}, 0x200001, depth_only);
        RenderState write_without_test;
        write_without_test.depth_test = false;
        square(pass, Bounds{31, 31, 33, 33}, Color{255, 255, 0}, 0xc00000, write_without_test);
        RenderState overlay = overlay_state(Color{255, 255, 255});
        square(pass, Bounds{0, 0, 24, 24}, Color{255, 255, 255}, 0, overlay);
        RenderState noop;
        noop.color_write = false;
        noop.depth_test = false;
        noop.depth_write = false;
        square(pass, Bounds{0, 0, 41, 41}, Color{0, 0, 0}, 0, noop);
        compare_pass(pass);
    });

    run_case("block Q16 depth rounding and saturation", [] {
        Pass pass;
        RenderState state;
        state.depth_func = DepthFunc::Always;
        const std::array<std::int64_t, 6> seeds{{
            -65536, 32768, 98304, 0x800000LL * 65536 + 32768,
            static_cast<std::int64_t>(depth_max) * 65536,
            static_cast<std::int64_t>(depth_max + 7u) * 65536}};
        const std::array<std::uint32_t, 6> expected{{0, 0, 2, 0x800000, depth_max, depth_max}};
        for (int i = 0; i < static_cast<int>(seeds.size()); ++i) {
            auto prepared = flat_triangle({{{i * 3, 0}, {i * 3 + 2, 0}, {i * 3, 2}}},
                                          Color{255, 255, 255}, 0, state);
            prepared.z = Plane{seeds[static_cast<std::size_t>(i)], 0, 0};
            pass.primitives.emplace_back(prepared);
        }
        compare_pass(pass);
        Renderer actual;
        actual.render(pass, RenderMode::HardwareBlocks);
        for (int i = 0; i < static_cast<int>(expected.size()); ++i) {
            expect(actual.depth()[pixel_index(i * 3, 0)] == expected[static_cast<std::size_t>(i)],
                   "fixed-plane depth conversion must be RNE then clamp to D24");
        }
    });

    run_case("block signed 64-bit planes saturate before narrowing", [] {
        Pass pass;
        RenderState state;
        state.shade = ShadeMode::Gouraud;
        state.depth_func = DepthFunc::Always;
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        auto high = flat_triangle({{{1, 1}, {3, 1}, {1, 3}}}, Color{}, 0, state);
        high.z = Plane{maximum, 0, 0};
        high.r = Plane{maximum, 0, 0};
        high.g = Plane{-maximum, 0, 0};
        high.b = Plane{128LL * 65536 + 32768, 0, 0};
        auto low = flat_triangle({{{5, 1}, {7, 1}, {5, 3}}}, Color{}, 0, state);
        low.z = Plane{-maximum, 0, 0};
        low.r = Plane{-maximum, 0, 0};
        low.g = Plane{maximum, 0, 0};
        low.b = Plane{0, 0, 0};
        pass.primitives = {high, low};
        compare_pass(pass);
        Renderer actual;
        actual.render(pass, RenderMode::HardwareBlocks);
        expect(actual.depth()[pixel_index(1, 1)] == depth_max &&
               actual.color(1)[pixel_index(1, 1)] == 0xf810,
               "large positive planes clamp and the half-even blue channel rounds to 128");
        expect(actual.depth()[pixel_index(5, 1)] == 0 &&
               actual.color(1)[pixel_index(5, 1)] == 0x07e0,
               "large negative planes clamp without signed narrowing or wrap");
    });

    run_case("block LOAD passes preserve physical color and shared depth across swaps", [] {
        Renderer expected;
        Renderer actual;
        Pass first;
        first.clear_color = 0x0123;
        square(first, Bounds{13, 15, 101, 107}, Color{255, 127, 31}, 0x812345);
        expected.render(first);
        actual.render(first, RenderMode::HardwareBlocks);
        compare_renderers(expected, actual);
        Pass second;
        second.color_load = LoadOp::Load;
        second.depth_load = LoadOp::Load;
        square(second, Bounds{63, 31, 131, 97}, Color{31, 127, 255}, 0x712345);
        expected.render(second);
        actual.render(second, RenderMode::HardwareBlocks);
        compare_renderers(expected, actual);
        expected.queue_present(1); actual.queue_present(1);
        expected.vertical_blank(); actual.vertical_blank();
        expect(actual.front_id() == 1 && actual.back_id() == 0, "presentation swaps color ownership");
        Pass third;
        third.target = 0;
        third.color_load = LoadOp::Load;
        third.depth_load = LoadOp::Load;
        square(third, Bounds{41, 29, 119, 111}, Color{13, 249, 97}, 0x912345);
        expected.render(third);
        actual.render(third, RenderMode::HardwareBlocks);
        compare_renderers(expected, actual);
        const auto previous_depth = actual.depth();
        expected.queue_present(0); actual.queue_present(0);
        expected.vertical_blank(); actual.vertical_blank();
        expect(actual.depth() == previous_depth, "presentation must not swap shared depth");
        Pass empty;
        empty.target = 1;
        empty.color_load = LoadOp::Load;
        empty.depth_load = LoadOp::Load;
        expected.render(empty);
        actual.render(empty, RenderMode::HardwareBlocks);
        compare_renderers(expected, actual);
    });

    run_case("block adapter honors independent color and depth load operations", [] {
        Renderer expected;
        Renderer actual;
        Pass initial;
        square(initial, Bounds{0, 0, 91, 77}, Color{255, 0, 0}, 0x800000);
        expected.render(initial);
        actual.render(initial, RenderMode::HardwareBlocks);
        Pass clear_depth;
        clear_depth.color_load = LoadOp::Load;
        clear_depth.depth_load = LoadOp::Clear;
        clear_depth.clear_depth = 0x100001;
        expected.render(clear_depth);
        actual.render(clear_depth, RenderMode::HardwareBlocks);
        compare_renderers(expected, actual);
        Pass clear_color;
        clear_color.color_load = LoadOp::Clear;
        clear_color.depth_load = LoadOp::Load;
        clear_color.clear_color = 0xace1;
        expected.render(clear_color);
        actual.render(clear_color, RenderMode::HardwareBlocks);
        compare_renderers(expected, actual);
    });

    run_case("block seeded mixed flat and Gouraud workload", [] {
        std::mt19937 random(0x20c2350u);
        Pass pass;
        for (int i = 0; i < 48; ++i) {
            const int x = static_cast<int>(random() % 580u);
            const int y = static_cast<int>(random() % 420u);
            const int width = 2 + static_cast<int>(random() % 58u);
            const int height = 2 + static_cast<int>(random() % 58u);
            std::array<ScreenVertex, 3> vertices{{
                {x + 0.0625L * (i % 16), y + 0.1875L, 0.2L + (random() % 50u) / 100.0L},
                {x + width + 0.0625L, static_cast<long double>(y), 0.2L + (random() % 50u) / 100.0L},
                {static_cast<long double>(x), y + height + 0.0625L, 0.2L + (random() % 50u) / 100.0L}}};
            for (auto& vertex : vertices) {
                vertex.r = random() % 256u;
                vertex.g = random() % 256u;
                vertex.b = random() % 256u;
            }
            RenderState state;
            state.shade = i % 2 == 0 ? ShadeMode::Flat : ShadeMode::Gouraud;
            state.color = Color{static_cast<std::uint8_t>(random()), static_cast<std::uint8_t>(random()),
                                static_cast<std::uint8_t>(random())};
            state.depth_func = i % 3 == 0 ? DepthFunc::LessEqual : DepthFunc::Less;
            state.depth_write = i % 7 != 0;
            state.color_write = i % 11 != 0;
            if (i % 4 == 0) state.scissor = Bounds{x + 1, y + 1, x + width, y + height};
            auto prepared = setup_triangle(vertices, state);
            if (prepared) pass.primitives.emplace_back(*prepared);
        }
        expect(pass.primitives.size() >= 40, "seeded workload must exercise many prepared records");
        compare_pass(pass);
    });

    run_case("block adapter rejects unsupported records before attachment writes", [] {
        Renderer actual;
        const auto color_before = actual.color(1);
        const auto depth_before = actual.depth();
        Pass pass;
        pass.clear_color = 0xffff;
        square(pass, Bounds{0, 0, 10, 10}, Color{255, 0, 0}, 0);
        pass.primitives.emplace_back(Rect{});
        expect_throws([&] { actual.render(pass, RenderMode::HardwareBlocks); },
                      "first slice must reject unimplemented 2D records");
        expect(actual.color(1) == color_before && actual.depth() == depth_before,
               "all record support must be checked before clear or earlier valid draws");
        expect(!actual.color_valid(1) && !actual.depth_valid(),
               "failed pass follows Renderer recovery rules");
        Pass recover;
        actual.render(recover, RenderMode::HardwareBlocks);
        expect(actual.color_valid(1) && actual.depth_valid(), "CLEAR recovers after an unsupported pass");
    });

    run_case("block adapter rejects texture shading explicitly", [] {
        Renderer actual;
        Pass pass;
        auto image = std::make_shared<Image>(1, 1);
        image->at(0, 0) = 0xffff;
        RenderState state;
        state.shade = ShadeMode::TextureReplace;
        state.texture = image;
        pass.primitives.emplace_back(flat_triangle({{{0, 0}, {4, 0}, {0, 4}}}, Color{}, 0, state));
        expect_throws([&] { actual.render(pass, RenderMode::HardwareBlocks); },
                      "hardware model must not silently delegate textures to canonical SRR");
    });
}

void finish(OpaqueTile& tile, bool insert_stalls = false, std::uint64_t* stalled = nullptr) {
    for (std::uint64_t clock = 0; tile.busy(); ++clock) {
        if (clock >= 200000) throw std::runtime_error("opaque tile failed to make bounded progress");
        const bool enabled = !insert_stalls || (clock % 5 != 1 && clock % 7 != 3);
        tile.tick(enabled);
        if (!enabled && stalled) ++*stalled;
    }
}

void compare_images(const TileImage& expected, const TileImage& actual) {
    expect(actual.color == expected.color, "tile color differs");
    expect(actual.depth == expected.depth, "tile depth differs");
}

TileImage pattern_tile() {
    TileImage image;
    for (std::size_t i = 0; i < image.color.size(); ++i) {
        image.color[i] = static_cast<std::uint16_t>((i * 103u) ^ 0x6ac5u);
        image.depth[i] = static_cast<std::uint32_t>((i * 17011u + 0x830051u) & depth_max);
    }
    return image;
}

void tile_tests() {
    run_case("tile load and store use clocked ports for all depth bits", [] {
        OpaqueTile tile;
        tile.reset();
        expect(tile.statistics().cycles == 1, "reset is one accounted rising edge");
        const TileImage image = pattern_tile();
        const auto before_load = tile.statistics().cycles;
        tile.load(image);
        expect(tile.statistics().cycles - before_load == 1024 && tile.statistics().load_words == 1024,
               "1024-word load must not write a host array for free");
        const auto before_store = tile.statistics().cycles;
        compare_images(image, tile.store());
        expect(tile.statistics().cycles - before_store == 1025 && tile.statistics().store_words == 1024,
               "store accounts for the synchronous read requests and output drain");
    });

    run_case("tile clear is a write sweep rather than a primitive reset", [] {
        OpaqueTile tile;
        tile.load(pattern_tile());
        const auto before = tile.statistics().cycles;
        tile.clear(0x96a5, 0x12abef);
        expect(tile.statistics().cycles - before == 1024, "clear costs one write edge per tile word");
        const auto image = tile.store();
        for (std::size_t i = 0; i < image.color.size(); ++i) {
            expect(image.color[i] == 0x96a5 && image.depth[i] == 0x12abef,
                   "clear must cover all three physical banks");
        }
        tile.reset();
        compare_images(image, tile.store());
    });

    run_case("tile input validation cannot truncate D24 or alias tile origins", [] {
        OpaqueTile tile;
        const TileImage image = pattern_tile();
        tile.load(image);
        auto invalid = image;
        invalid.depth.back() = 0x01000000;
        expect_throws([&] { tile.load(invalid); }, "late out-of-range D24 word rejected before load starts");
        compare_images(image, tile.store());
        expect_throws([&] { tile.clear(0, 0x01000000); }, "D24 clear cannot silently truncate");
        compare_images(image, tile.store());
        const auto prepared = flat_triangle({{{0, 0}, {10, 0}, {0, 10}}}, Color{}, 0);
        expect_throws([&] { tile.begin(prepared, Point{1, 0}); }, "origin must align to a physical logical tile");
        expect_throws([&] { tile.begin(prepared, Point{640, 0}); }, "origin must lie inside the visible tile grid");
        expect(!tile.busy(), "invalid commands must not claim the tile");
    });

    run_case("tile pipeline stalls preserve data, order, and enabled work", [] {
        OpaqueTile normal;
        OpaqueTile stalled;
        const TileImage initial = pattern_tile();
        normal.load(initial);
        stalled.load(initial);
        RenderState state;
        state.shade = ShadeMode::Gouraud;
        state.depth_func = DepthFunc::Always;
        std::array<ScreenVertex, 3> vertices{{{18.25L, 7.125L, 0.1L},
                                             {95.125L, 31.375L, 0.9L},
                                             {37.75L, 99.875L, 0.3L}}};
        vertices[0].r = 255; vertices[0].g = 9; vertices[0].b = 31;
        vertices[1].r = 11; vertices[1].g = 255; vertices[1].b = 0;
        vertices[2].r = 0; vertices[2].g = 17; vertices[2].b = 255;
        const auto prepared = triangle(vertices, state);
        normal.begin(prepared, Point{32, 32});
        stalled.begin(prepared, Point{32, 32});
        finish(normal);
        std::uint64_t stalls = 0;
        finish(stalled, true, &stalls);
        compare_images(normal.store(), stalled.store());
        const auto& expected = normal.statistics();
        const auto& actual = stalled.statistics();
        expect(stalls > 0 && actual.stalls == stalls && actual.cycles == expected.cycles + stalls,
               "stalls add clocks without advancing work");
        expect(actual.setup_cycles == expected.setup_cycles && actual.candidates == expected.candidates &&
               actual.covered_fragments == expected.covered_fragments &&
               actual.depth_rejected == expected.depth_rejected &&
               actual.color_writes == expected.color_writes && actual.depth_writes == expected.depth_writes,
               "stalling must not duplicate, skip, or reorder fragments");
    });

    run_case("tile repeated one-pixel overdraw observes the latest stored depth", [] {
        OpaqueTile tile;
        tile.clear(0, depth_max);
        const std::array<std::uint32_t, 5> depths{{0x800001, 0x800000, 0x800000, 0x800002, 0x7fffff}};
        const std::array<Color, 5> colors{{{255, 0, 0}, {0, 255, 0}, {0, 0, 255},
                                         {255, 255, 255}, {255, 255, 0}}};
        for (std::size_t i = 0; i < depths.size(); ++i) {
            auto prepared = flat_triangle({{{32, 32}, {34, 32}, {32, 34}}}, colors[i], depths[i]);
            tile.begin(prepared, Point{32, 32});
            finish(tile, i % 2 != 0);
        }
        const auto image = tile.store();
        expect(image.depth[0] == 0x7fffff && image.color[0] == 0xffe0,
               "same-pixel dependencies must preserve low D24 bits and command order");
        expect(tile.statistics().covered_fragments == 5 && tile.statistics().depth_rejected == 2,
               "equal and farther fragments fail while strictly nearer fragments pass");
    });

    run_case("tile lifecycle forbids replacing in-flight data and reset cancels setup", [] {
        OpaqueTile tile;
        const TileImage original = pattern_tile();
        tile.load(original);
        const auto prepared = flat_triangle({{{0, 0}, {32, 0}, {0, 32}}}, Color{255, 0, 0}, 0);
        tile.begin(prepared, Point{0, 0});
        expect(tile.busy(), "accepted nonempty draw owns the tile");
        expect_throws([&] { tile.load(original); }, "load must wait for drawing");
        expect_throws([&] { tile.clear(0, 0); }, "clear must wait for drawing");
        expect_throws([&] { (void)tile.store(); }, "store must wait for pending writes");
        expect_throws([&] { tile.begin(prepared, Point{0, 0}); }, "draw commands require ready state");
        tile.tick();
        tile.reset();
        expect(!tile.busy(), "reset cancels the in-flight command");
        compare_images(original, tile.store());
        tile.begin(prepared, Point{0, 0});
        finish(tile);
        expect(tile.statistics().color_writes > 0, "tile can make forward progress after reset");
    });
}

} // namespace

int main() {
    multiplier_tests();
    bsram_tests();
    resource_tests();
    renderer_tests();
    tile_tests();
    std::cout << cases_run - cases_failed << '/' << cases_run << " hardware model cases passed\n";
    return cases_failed == 0 ? 0 : 1;
}
