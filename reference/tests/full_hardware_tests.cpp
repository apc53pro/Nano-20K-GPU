#include "nano_gpu/demo.hpp"
#include "nano_gpu/hw/device.hpp"
#include "nano_gpu/hw/fragment_pipeline.hpp"
#include "nano_gpu/hw/perspective.hpp"
#include "nano_gpu/hw/reciprocal.hpp"
#include "nano_gpu/hw/sdram.hpp"
#include "nano_gpu/hw/texture.hpp"
#include "nano_gpu/hw/tile_pipeline.hpp"
#include "nano_gpu/hw/walker2d.hpp"
#include "nano_gpu/reference.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
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
    try { function(); } catch (const std::exception&) { threw = true; }
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

std::size_t index(int x, int y) { return static_cast<std::size_t>(y) * screen_width + x; }

Triangle prepared(const std::array<ScreenVertex, 3>& vertices, const RenderState& state) {
    auto result = setup_triangle(vertices, state);
    if (!result) throw std::runtime_error("test triangle unexpectedly culled");
    return *result;
}

std::shared_ptr<Image> pattern(int width = 16, int height = 16, int pitch = 0) {
    auto image = std::make_shared<Image>(width, height, pitch);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image->at(x, y) = static_cast<std::uint16_t>(((x * 7 + y * 3) & 31) << 11 |
                ((x * 5 + y * 11) & 63) << 5 | ((x * 13 + y * 17) & 31));
    return image;
}

std::shared_ptr<Image> solid(std::uint16_t value) {
    auto image = std::make_shared<Image>(1, 1);
    image->at(0, 0) = value;
    return image;
}

void compare_outputs(const Renderer& expected, const Renderer& actual) {
    expect(expected.color(0) == actual.color(0), "physical color buffer 0 differs");
    expect(expected.color(1) == actual.color(1), "physical color buffer 1 differs");
    expect(expected.depth() == actual.depth(), "D24 depth differs");
    const auto& e = expected.statistics();
    const auto& a = actual.statistics();
    expect(a.candidates == e.candidates, "candidate-position counts differ");
    expect(a.covered_fragments == e.covered_fragments, "covered-fragment counts differ");
    expect(a.depth_rejected == e.depth_rejected, "depth-rejection counts differ");
    expect(a.keyed_rejected == e.keyed_rejected, "key-rejection counts differ");
    expect(a.texture_samples == e.texture_samples, "texture-sample counts differ");
    expect(a.color_writes == e.color_writes && a.depth_writes == e.depth_writes,
           "attachment-write counts differ");
    expect(a.tile_references == e.tile_references && a.tiles_processed == tile_count,
           "tile-list processing differs");
    expect(a.hardware_cycles != 0, "block execution must report modeled clock edges");
}

Renderer compare_pass(const Pass& pass) {
    Renderer expected, actual;
    expected.render(pass, RenderMode::Tiled);
    actual.render(pass, RenderMode::HardwareBlocks);
    compare_outputs(expected, actual);
    return actual;
}

using Wide = __int128;
std::int32_t oracle_uv(std::int64_t value, std::int64_t reciprocal) {
    const Wide signed_numerator = static_cast<Wide>(value) * (Wide{1} << 26);
    const bool negative = signed_numerator < 0;
    const Wide numerator = negative ? -signed_numerator : signed_numerator;
    Wide quotient = numerator / reciprocal;
    const Wide remainder = numerator % reciprocal;
    if (2 * remainder > reciprocal || (2 * remainder == reciprocal && (quotient & 1))) ++quotient;
    const Wide result = negative ? -quotient : quotient;
    if (result < std::numeric_limits<std::int32_t>::min() ||
        result > std::numeric_limits<std::int32_t>::max()) throw std::overflow_error("oracle UV overflow");
    return static_cast<std::int32_t>(result);
}

void check_division(std::int64_t value, std::int64_t reciprocal, bool stall) {
    PerspectiveDivider divider;
    std::int32_t expected = 0;
    bool overflow = false;
    try { expected = oracle_uv(value, reciprocal); }
    catch (const std::overflow_error&) { overflow = true; }
    divider.start(value, reciprocal);
    bool rejected = false;
    for (unsigned clock = 0; divider.busy(); ++clock) {
        if (clock > 1000) throw std::runtime_error("perspective divider made no bounded progress");
        const bool enabled = !stall || clock % 5 != 1;
        const auto before = divider.elapsed_edges();
        try { divider.tick(enabled); }
        catch (const std::overflow_error&) { rejected = true; }
        if (!enabled) expect(divider.elapsed_edges() == before, "disabled divider edge changed state");
    }
    expect(rejected == overflow, "divider overflow differs from the independent 128-bit oracle");
    expect(divider.elapsed_edges() == PerspectiveDivider::enabled_edge_latency,
           "divider must use the documented bounded serial schedule");
    if (overflow) {
        expect(!divider.result_valid(), "overflow exposed a valid truncated result");
        expect_throws([&] { (void)divider.result(); }, "overflow result must not be observable");
    } else {
        expect(divider.result_valid() && divider.result() == expected,
               "serial division/rounding differs from independent integer arithmetic");
    }
}

void divider_tests() {
    run_case("perspective divider signed ties and representable endpoints", [] {
        constexpr std::int64_t q = std::int64_t{1} << 29;
        for (const auto value : {0LL, 1LL, 4LL, 12LL, 20LL, -1LL, -4LL, -12LL, -20LL,
                17179869176LL, 17179869180LL, -17179869184LL, -17179869188LL,
                -17179869189LL}) check_division(value, q, true);
        check_division(std::numeric_limits<std::int64_t>::min(), q, false);
        check_division(std::numeric_limits<std::int64_t>::max(), q, false);
    });
    run_case("perspective divider random exact-ratio comparison across supported q", [] {
        std::mt19937_64 random(0x514e365550455253ULL);
        constexpr std::uint64_t low = std::uint64_t{1} << 29;
        constexpr std::uint64_t high = std::uint64_t{1} << 37;
        for (int i = 0; i < 2048; ++i) {
            const auto q = static_cast<std::int64_t>(low + random() % (high - low + 1));
            auto value = static_cast<std::int64_t>(random() & ((std::uint64_t{1} << 40) - 1));
            if (i & 1) value = -value;
            check_division(value, q, i % 3 == 0);
        }
    });
    run_case("perspective divider capture validation and reset cancel", [] {
        PerspectiveDivider divider;
        expect(!divider.result_valid() && !divider.busy(), "new divider has no completed request");
        expect_throws([&] { divider.start(1, 0); }, "zero q must be rejected");
        expect_throws([&] { divider.start(1, (std::int64_t{1} << 29) - 1); }, "small q must be rejected");
        expect_throws([&] { divider.start(1, (std::int64_t{1} << 37) + 1); }, "large q must be rejected");
        divider.start(48, std::int64_t{1} << 29);
        divider.tick();
        const auto before = divider.elapsed_edges();
        expect_throws([&] { divider.start(9, std::int64_t{1} << 29); }, "active request cannot be replaced");
        expect(divider.busy() && divider.elapsed_edges() == before, "bad capture disturbed active request");
        divider.reset();
        expect(!divider.busy() && !divider.result_valid(), "reset must cancel pending UV");
        check_division(-48, std::int64_t{1} << 29, true);
    });
}

std::uint16_t sample(TextureUnit& unit, int x, int y, bool stall = false) {
    unit.start(x, y);
    for (unsigned clock = 0; unit.busy(); ++clock) {
        if (clock > 1000) throw std::runtime_error("texture unit made no bounded progress");
        unit.tick(!stall || clock % 4 != 1);
    }
    expect(unit.result_valid(), "completed texture request has no result");
    return unit.result();
}

void texture_tests() {
    run_case("texture cache miss refill, hit, and direct-map conflict", [] {
        auto image = pattern(64, 64);
        TextureUnit unit;
        unit.bind(image);
        const auto before = unit.statistics();
        expect(sample(unit, 0, 0) == image->at(0, 0), "first miss returned wrong texel");
        expect(unit.statistics().misses == before.misses + 1 &&
               unit.statistics().refill_words == before.refill_words + TextureUnit::line_words,
               "miss must fetch precisely one bounded cache line");
        const auto miss_edges = unit.statistics().cycles - before.cycles;
        const auto after_miss = unit.statistics();
        expect(sample(unit, 1, 0) == image->at(1, 0), "cache hit returned wrong lane");
        expect(unit.statistics().hits == after_miss.hits + 1 &&
               unit.statistics().refill_words == after_miss.refill_words, "cache hit refilled memory");
        const auto hit_edges = unit.statistics().cycles - after_miss.cycles;
        expect(miss_edges == hit_edges + TextureUnit::backing_response_edges + TextureUnit::line_words,
               "miss latency must include response wait and clocked line refill");
        expect(sample(unit, 0, 16) == image->at(0, 16), "conflicting tag returned stale texel");
        expect(sample(unit, 0, 0) == image->at(0, 0), "evicted line was not refetched");
        expect(unit.statistics().misses == before.misses + 3, "direct-map replacement was not visible");
    });
    run_case("texture cache explicit stride and padding preserve pixel addressing", [] {
        auto image = pattern(8, 8, 24);
        TextureUnit unit;
        unit.bind(image);
        for (int y = 0; y < image->height(); ++y)
            for (int x = 0; x < image->width(); ++x)
                expect(sample(unit, x, y, (x + y) % 3 == 0) == image->at(x, y),
                       "stride/padding changed an addressed source pixel");
        expect(unit.statistics().stalls != 0, "test must exercise stalled refill/read state");
    });
    run_case("texture cache binding, invalidation and reset visibility", [] {
        auto first = pattern(16, 16);
        auto second = solid(0x9d73);
        TextureUnit unit;
        unit.bind(first);
        expect(sample(unit, 3, 2) == first->at(3, 2), "first binding failed");
        const auto misses = unit.statistics().misses;
        unit.bind(first);
        expect(sample(unit, 3, 2) == first->at(3, 2) && unit.statistics().misses == misses,
               "rebinding same immutable image discarded a useful line");
        first->at(3, 2) = 0x3b75;
        unit.invalidate();
        expect(sample(unit, 3, 2) == 0x3b75, "explicit invalidation did not expose uploaded texel");
        unit.bind(second);
        expect(sample(unit, 0, 0) == 0x9d73, "binding changed without invalidating old tags");
        unit.reset();
        expect(!unit.busy() && !unit.result_valid(), "reset preserved a pending/result transaction");
        expect_throws([&] { unit.start(0, 0); }, "reset must unbind the old descriptor");
        unit.bind(first);
        expect(sample(unit, 3, 2) == 0x3b75 && unit.statistics().misses == 1,
               "reset recovery relied on stale cache RAM/tags");
    });
    run_case("texture request validation preserves in-flight state", [] {
        auto image = pattern(16, 16);
        TextureUnit unit;
        expect_throws([&] { (void)unit.result(); }, "no completed request may be observed");
        expect_throws([&] { unit.bind(std::make_shared<Image>(3, 4)); }, "non-POT texture accepted");
        unit.bind(image);
        expect_throws([&] { unit.start(-1, 0); }, "negative coordinate accepted");
        expect_throws([&] { unit.start(0, 16); }, "out-of-range coordinate accepted");
        unit.start(7, 9);
        unit.tick();
        expect_throws([&] { unit.start(0, 0); }, "active texture request replaced");
        expect_throws([&] { unit.bind(solid(0)); }, "active descriptor rebound");
        for (unsigned guard = 0; unit.busy(); ++guard) {
            expect(guard < 1000, "rejected operation stalled valid texture request");
            unit.tick();
        }
        expect(unit.result() == image->at(7, 9), "invalid command mutated the active request");
    });    run_case("external texture refill streams native slot words and pads a short final line", [] {
        SdramController memory;
        for (unsigned guard = 0; !memory.initialized(); ++guard) {
            expect(guard < 100000, "external texture SDRAM initialization hung");
            memory.tick();
        }
        const TextureDescriptor descriptor{memory_bytes - 8, 2, 2, 4};
        expect(memory.write_request_word(MemoryClient::Host, 0, 0x22221111u, 0xf),
               "texture seed first native word was blocked");
        memory.tick();
        expect(memory.write_request_word(MemoryClient::Host, 1, 0x44443333u, 0xf),
               "texture seed second native word was blocked");
        memory.tick();
        MemoryRequest upload; upload.write = true; upload.address = descriptor.base; upload.words = 2;
        expect(memory.submit_prepared(MemoryClient::Host, upload), "texture native seed could not submit");
        for (unsigned guard = 0; !memory.response_ready(MemoryClient::Host); ++guard) {
            expect(guard < 10000, "external texture seed did not complete"); memory.tick();
        }
        memory.release_response(MemoryClient::Host);
        const auto previous_slot_reads = memory.statistics().slot_read_words;
        TextureUnit unit;
        unit.bind_external(descriptor, memory); memory.tick();
        unit.start(1, 1); memory.tick();
        bool observed_pending = false, observed_slot_read = false;
        std::uint64_t ready_cycle = 0, first_refill_cycle = 0;
        for (unsigned guard = 0; unit.busy(); ++guard) {
            expect(guard < 10000, "native texture refill hung");
            if (unit.external_memory_pending()) {
                observed_pending = true;
                expect_throws([&] { unit.reset(); }, "texture reset abandoned an owned native response");
            }
            if (memory.response_ready(MemoryClient::Texture) && ready_cycle == 0)
                ready_cycle = unit.statistics().cycles;
            unit.tick(guard % 5 != 1); memory.tick();
            if (unit.statistics().refill_words != 0 && first_refill_cycle == 0)
                first_refill_cycle = unit.statistics().cycles;
            if (memory.statistics().slot_read_words > previous_slot_reads) observed_slot_read = true;
        }
        const auto& counts = memory.statistics();
        expect(observed_pending && observed_slot_read && ready_cycle != 0 &&
               first_refill_cycle >= ready_cycle + 3,
               "texture refill skipped slot read or separate word-capture edges");
        expect(unit.result_valid() && unit.result() == 0x4444 && !unit.external_memory_pending(),
               "native short-line refill returned an incorrect texel or retained ownership");
        expect(unit.statistics().refill_words == TextureUnit::line_words &&
               counts.slot_read_words - previous_slot_reads == 2 &&
               counts.clients[static_cast<unsigned>(MemoryClient::Texture)].bytes_read == 8,
               "short final line read beyond its allocation instead of padding the cache tail");
        expect(counts.legacy_submissions == 0 && counts.legacy_response_reads == 0,
               "external texture used a host-only bulk transfer adapter");
    });

}

std::vector<Point> walk(const Primitive& primitive, Point origin, bool stall = false) {
    Walker2D walker;
    walker.begin(primitive, origin);
    std::vector<Point> result;
    for (unsigned clock = 0; walker.busy(); ++clock) {
        expect(clock < 10000, "2D walker made no bounded progress");
        const auto before = walker.sample();
        if (stall && clock % 3 == 1) {
            walker.tick(false);
            const auto after = walker.sample();
            expect(before.x == after.x && before.y == after.y && before.covered == after.covered &&
                   before.source_x == after.source_x && before.source_y == after.source_y,
                   "disabled 2D edge advanced sample state");
        } else {
            if (before.covered) result.push_back({before.x, before.y});
            walker.tick();
        }
    }
    return result;
}

void walker_tests() {
    run_case("line walker keeps global Bresenham ties across tiles and reversal", [] {
        for (const auto& endpoints : std::array<std::pair<Point, Point>, 5>{{
                 {{0, 0}, {3, 1}}, {{4, 47}, {127, 10}}, {{47, 4}, {10, 127}},
                 {{31, 31}, {65, 65}}, {{63, 47}, {63, 47}}}}) {
            Line line;
            line.a = endpoints.first;
            line.b = endpoints.second;
            line.state.scissor = {1, 0, 120, 120};
            for (int ty = 0; ty < 4; ++ty) {
                for (int tx = 0; tx < 4; ++tx) {
                    const Point origin{tx * tile_size, ty * tile_size};
                    const Bounds clip = intersect(line.state.scissor,
                        Bounds{origin.x, origin.y, origin.x + tile_size, origin.y + tile_size});
                    std::vector<Point> expected;
                    for (const auto p : line_pixels(line.a, line.b))
                        if (clip.contains(p.x, p.y)) expected.push_back(p);
                    expect(walk(line, origin, true) == expected, "tile-local line changed global ties");
                    std::swap(line.a, line.b);
                    expect(walk(line, origin) == expected, "reversing line changed global point order");
                }
            }
        }
    });
    run_case("sprite walker preserves source offsets after negative-origin clipping", [] {
        Sprite sprite;
        sprite.bounds = {-3, -2, 7, 6};
        sprite.source_x = 2;
        sprite.source_y = 1;
        sprite.state.texture = pattern(16, 16);
        sprite.state.scissor = {1, 1, 6, 5};
        Walker2D walker;
        walker.begin(sprite, {0, 0});
        unsigned samples = 0;
        while (walker.busy()) {
            const auto p = walker.sample();
            expect(p.covered && p.x >= 1 && p.x < 6 && p.y >= 1 && p.y < 5,
                   "sprite scissor/window differs");
            expect(p.source_x == p.x + 5 && p.source_y == p.y + 3,
                   "sprite source was incorrectly restarted at clipped origin");
            ++samples;
            walker.tick();
        }
        expect(samples == 20, "clipped sprite sample count differs");
    });
    run_case("2D walker rejects unsupported state before sampling", [] {
        Walker2D walker;
        Line invalid;
        invalid.b = {640, 0};
        expect_throws([&] { walker.begin(invalid, {0, 0}); }, "off-screen line endpoint accepted");
        Rect rect;
        rect.state.shade = ShadeMode::TextureReplace;
        expect_throws([&] { walker.begin(rect, {0, 0}); }, "textured rectangle accepted");
        Sprite sprite;
        sprite.bounds = {0, 0, 2, 2};
        sprite.source_x = 15;
        sprite.state.texture = pattern(16, 16);
        expect_throws([&] { walker.begin(sprite, {0, 0}); }, "sprite source outside image accepted");
    });
}

std::array<ScreenVertex, 3> textured_vertices(int x = 24, int y = 24) {
    return {{{static_cast<long double>(x), static_cast<long double>(y), 0.22L, 1,
               -4.75L, -1.5L, 255, 80, 32},
             {static_cast<long double>(x + 45), static_cast<long double>(y + 3), 0.42L, 4,
               35.25L, 2.25L, 40, 230, 110},
             {static_cast<long double>(x + 6), static_cast<long double>(y + 43), 0.32L, 2,
               -2.5L, 32.75L, 64, 120, 250}}};
}

void rendering_tests() {
    run_case("all five shading modes share exact prepared inputs with canonical SRR", [] {
        Pass pass;
        const std::array<ShadeMode, 5> modes{{ShadeMode::Flat, ShadeMode::Gouraud,
            ShadeMode::TextureReplace, ShadeMode::TextureFlatModulate, ShadeMode::TextureGouraudModulate}};
        auto image = pattern(16, 16, 40);
        for (std::size_t i = 0; i < modes.size(); ++i) {
            RenderState state;
            state.shade = modes[i];
            state.color = {128, 64, 32};
            state.address_u = i & 1 ? AddressMode::Clamp : AddressMode::Repeat;
            state.address_v = i & 1 ? AddressMode::Repeat : AddressMode::Clamp;
            if (i >= 2) state.texture = image;
            pass.primitives.emplace_back(prepared(textured_vertices(20 + static_cast<int>(i) * 80, 24), state));
        }
        const auto actual = compare_pass(pass);
        expect(actual.statistics().texture_samples > 0, "textured triangles did not sample");
    });
    run_case("perspective q extremes and negative repeat boundaries", [] {
        Pass pass;
        RenderState state;
        state.shade = ShadeMode::TextureReplace;
        state.texture = pattern(8, 8);
        state.address_u = state.address_v = AddressMode::Repeat;
        auto vertices = textured_vertices();
        vertices[0].w = 1;
        vertices[1].w = 64;
        vertices[2].w = 8;
        vertices[0].u = -8.0000152587890625L;
        vertices[1].u = 7.9999847412109375L;
        vertices[2].v = -0.0000152587890625L;
        pass.primitives.emplace_back(prepared(vertices, state));
        compare_pass(pass);
    });
    run_case("raw color key precedes modulation and both attachment commits", [] {
        Pass pass;
        pass.clear_color = 0x1234;
        RenderState keyed;
        keyed.shade = ShadeMode::TextureFlatModulate;
        keyed.texture = solid(0xf81f);
        keyed.color_key = 0xf81f;
        keyed.color = {0, 0, 0};
        auto near_vertices = textured_vertices(10, 10);
        for (auto& v : near_vertices) v.depth = 0.1L;
        pass.primitives.emplace_back(prepared(near_vertices, keyed));
        keyed.texture = solid(0x07e0);
        keyed.color_key.reset();
        keyed.color = {255, 255, 255};
        for (auto& v : near_vertices) v.depth = 0.6L;
        pass.primitives.emplace_back(prepared(near_vertices, keyed));
        const auto actual = compare_pass(pass);
        expect(actual.statistics().keyed_rejected > 0, "keyed texels did not reject");
        expect(actual.color(1)[index(20, 20)] == 0x07e0,
               "keyed near texel wrote depth or key was compared after modulation");
    });
    run_case("color-disabled keyed texture still determines depth-only visibility", [] {
        Pass pass;
        pass.clear_color = 0x2345;
        RenderState state;
        state.shade = ShadeMode::TextureReplace;
        state.texture = solid(0xf81f);
        state.color_key = 0xf81f;
        state.color_write = false;
        auto vertices = textured_vertices(10, 10);
        for (auto& v : vertices) v.depth = 0.1L;
        pass.primitives.emplace_back(prepared(vertices, state));
        state.color_write = true;
        state.color_key.reset();
        state.texture = solid(0x001f);
        for (auto& v : vertices) v.depth = 0.7L;
        pass.primitives.emplace_back(prepared(vertices, state));
        const auto actual = compare_pass(pass);
        expect(actual.statistics().keyed_rejected > 0 && actual.statistics().texture_samples > 0,
               "depth-only key path skipped lookup/rejection");
        expect(actual.color(1)[index(20, 20)] == 0x001f, "keyed depth-only draw blocked farther opaque draw");
    });
    run_case("unkeyed color-disabled texture skips unnecessary sampling", [] {
        Pass pass;
        RenderState state;
        state.shade = ShadeMode::TextureGouraudModulate;
        state.texture = pattern();
        state.color_write = false;
        pass.primitives.emplace_back(prepared(textured_vertices(), state));
        const auto actual = compare_pass(pass);
        expect(actual.statistics().texture_samples == 0 && actual.statistics().depth_writes != 0,
               "depth-only opaque texture performed unnecessary sampling");
    });
    run_case("early depth rejects suppress perspective and texture work", [] {
        Pass pass;
        RenderState state;
        state.shade = ShadeMode::TextureReplace;
        state.texture = pattern();
        auto vertices = textured_vertices();
        for (auto& v : vertices) v.depth = 0.1L;
        pass.primitives.emplace_back(prepared(vertices, state));
        for (auto& v : vertices) v.depth = 0.9L;
        pass.primitives.emplace_back(prepared(vertices, state));
        const auto actual = compare_pass(pass);
        expect(actual.statistics().depth_rejected != 0 &&
               actual.statistics().texture_samples == actual.statistics().covered_fragments - actual.statistics().depth_rejected,
               "rejected depth still sampled textures");
    });
    run_case("mixed 2D records and triangles preserve clipping, source origins, and keying", [] {
        Pass pass;
        pass.clear_color = 0x1234;
        RenderState state;
        state.color = {230, 40, 80};
        pass.primitives.emplace_back(prepared(textured_vertices(18, 18), state));
        Rect rectangle;
        rectangle.bounds = {-15, 20, 48, 57};
        rectangle.state = overlay_state({25, 80, 140});
        rectangle.state.scissor = {2, 2, 50, 50};
        pass.primitives.emplace_back(rectangle);
        Line line;
        line.a = {1, 59};
        line.b = {98, 7};
        line.state = overlay_state({255, 220, 40});
        line.state.scissor = {5, 5, 92, 55};
        pass.primitives.emplace_back(line);
        auto image = pattern(16, 16, 40);
        image->at(7, 5) = 0xf81f;
        Sprite sprite;
        sprite.bounds = {-3, -2, 7, 6};
        sprite.source_x = 2;
        sprite.source_y = 1;
        sprite.state.texture = image;
        sprite.state.color_key = 0xf81f;
        pass.primitives.emplace_back(sprite);
        sprite.bounds = {29, 29, 39, 37};
        pass.primitives.emplace_back(sprite);
        const auto actual = compare_pass(pass);
        expect(actual.statistics().keyed_rejected > 0, "mixed sprite test did not exercise key rejection");
    });
    run_case("keyed sprite depth-only rejection and subsequent opaque depth", [] {
        Pass pass;
        Sprite sprite;
        sprite.bounds = {31, 31, 33, 33};
        sprite.state.texture = solid(0xf81f);
        // A 1x1 texture cannot cover 2x2 without scaling: use a 2x2 source.
        auto keyed_image = std::make_shared<Image>(2, 2);
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) keyed_image->at(x, y) = 0xf81f;
        sprite.state.texture = keyed_image;
        sprite.state.color_key = 0xf81f;
        sprite.state.color_write = false;
        sprite.state.depth_test = sprite.state.depth_write = true;
        sprite.depth = 0x001234;
        pass.primitives.emplace_back(sprite);
        Rect far;
        far.bounds = sprite.bounds;
        far.state = RenderState{};
        far.state.color = {0, 255, 0};
        far.depth = 0x800001;
        pass.primitives.emplace_back(far);
        const auto actual = compare_pass(pass);
        expect(actual.statistics().keyed_rejected == 4, "keyed sprite count differs at four-tile crossing");
        expect(actual.depth()[index(32, 32)] == 0x800001, "keyed sprite wrote provisional D24");
    });
    run_case("textured multipass LOAD and presentation preserve shared depth", [] {
        Renderer expected, actual;
        Pass first;
        RenderState state;
        state.shade = ShadeMode::TextureGouraudModulate;
        state.texture = pattern();
        first.primitives.emplace_back(prepared(textured_vertices(), state));
        expected.render(first);
        actual.render(first, RenderMode::HardwareBlocks);
        compare_outputs(expected, actual);
        Pass second;
        second.color_load = second.depth_load = LoadOp::Load;
        Rect rectangle;
        rectangle.bounds = {30, 30, 70, 40};
        rectangle.state = overlay_state({140, 210, 70});
        second.primitives.emplace_back(rectangle);
        expected.render(second);
        actual.render(second, RenderMode::HardwareBlocks);
        compare_outputs(expected, actual);
        expected.queue_present(1); actual.queue_present(1);
        expected.vertical_blank(); actual.vertical_blank();
        Pass third;
        third.target = 0;
        third.depth_load = LoadOp::Load;
        third.primitives.emplace_back(prepared(textured_vertices(28, 30), state));
        expected.render(third);
        actual.render(third, RenderMode::HardwareBlocks);
        compare_outputs(expected, actual);
    });
}

void copy_tests() {
    run_case("hardware row-buffer copies preserve horizontal and vertical overlap snapshots", [] {
        auto image = pattern(64, 64, 136);
        for (const auto displacement : std::array<Point, 4>{{{3, 0}, {-3, 0}, {0, 3}, {0, -3}}}) {
            Renderer expected, actual;
            expected.copy_image(*image, 1, {0, 0}, {12, 12}, 64, 64);
            actual.copy_image(*image, 1, {0, 0}, {12, 12}, 64, 64, RenderMode::HardwareBlocks);
            const auto z_before = actual.depth();
            expected.copy_rect(1, 1, {20, 20}, {20 + displacement.x, 20 + displacement.y}, 48, 48);
            actual.copy_rect(1, 1, {20, 20}, {20 + displacement.x, 20 + displacement.y}, 48, 48,
                             RenderMode::HardwareBlocks);
            expect(actual.color(1) == expected.color(1), "directional hardware overlap changed source snapshot");
            expect(actual.depth() == z_before, "color copy changed shared depth");
            expect(actual.statistics().hardware_copy_words == 48u * 48 &&
                   actual.statistics().hardware_copy_cycles == 1u + 48u * (2u * 48 + 1),
                   "row-buffer clock counts must cover writes, synchronous reads, and row drains");
        }
    });
    run_case("hardware copies clip NPOT images with padded source pitch", [] {
        auto image = pattern(23, 17, 52);
        Renderer expected, actual;
        expected.copy_image(*image, 1, {-2, -3}, {-4, 475}, 30, 25);
        actual.copy_image(*image, 1, {-2, -3}, {-4, 475}, 30, 25, RenderMode::HardwareBlocks);
        expect(actual.color(1) == expected.color(1), "clipped NPOT source/pitch mapping differs");
        expect(actual.depth() == expected.depth(), "NPOT color copy changed depth");
        expect(actual.statistics().hardware_copy_words != 0, "clipped copy did not exercise row buffer");
        expected.copy_rect(1, 1, {0, 475}, {635, 477}, 23, 17);
        actual.copy_rect(1, 1, {0, 475}, {635, 477}, 23, 17, RenderMode::HardwareBlocks);
        expect(actual.color(1) == expected.color(1), "right/bottom framebuffer clipping differs");
    });
    run_case("hardware copies obey front-buffer ownership and empty clipping", [] {
        Renderer expected, actual;
        auto image = pattern(8, 8);
        expect_throws([&] { actual.copy_image(*image, 0, {0, 0}, {0, 0}, 8, 8, RenderMode::HardwareBlocks); },
                      "displayed buffer must not accept hardware image writes");
        actual.copy_image(*image, 1, {0, 0}, {0, 0}, 8, 8, RenderMode::HardwareBlocks);
        expected.copy_image(*image, 1, {0, 0}, {0, 0}, 8, 8);
        actual.queue_present(1); actual.vertical_blank();
        expected.queue_present(1); expected.vertical_blank();
        actual.copy_rect(1, 0, {0, 0}, {31, 31}, 8, 8, RenderMode::HardwareBlocks);
        expected.copy_rect(1, 0, {0, 0}, {31, 31}, 8, 8);
        expect(actual.color(0) == expected.color(0), "front-to-back hardware copy differs");
        const auto before = actual.color(0);
        actual.copy_rect(0, 0, {0, 0}, {std::numeric_limits<int>::max(), 0}, 8, 8,
                         RenderMode::HardwareBlocks);
        expect(actual.color(0) == before, "fully clipped copy changed pixels");
    });
}

void tile_stall_tests() {
    run_case("streamed UV texture shader and context outputs cross registered clock boundaries", [] {
        RenderState state = overlay_state();
        state.shade = ShadeMode::TextureReplace;
        state.texture = solid(0x07e0);
        const std::array<ScreenVertex, 3> vertices{{{0, 0}, {2, 0}, {0, 2}}};
        OpaqueTile tile;
        tile.clear(0x1234, depth_max);
        const auto before = tile.statistics().cycles;

        // Independent interface schedule for one covered sample in a2x2 box.
        // UVlaunch shares the early-depth capture edge. UVparent capture also
        // launches the factor RAM read; texture parent capture launches shade.
        // These coincident launches still consume OLD registered outputs.
        constexpr unsigned command_capture = 1;
        constexpr unsigned edge_setup = 6 * 4; // 3 DSP stages + parent capture
        constexpr unsigned candidates = 4;
        constexpr unsigned warm_fragment = PerspectivePipeline::enabled_edge_latency +
            TexturedFragmentPipeline::factor_read_edges + TexturedFragmentPipeline::factor_capture_edges +
            TextureUnit::pipeline_hit_edges + TexturedFragmentPipeline::shade_enabled_edges +
            TexturedFragmentPipeline::retirement_read_edges + TexturedFragmentPipeline::retirement_capture_edges;
        constexpr unsigned cold_extra = TextureUnit::backing_response_edges + TextureUnit::line_words + 1;
        constexpr unsigned first_accept = command_capture + edge_setup + 1;
        constexpr unsigned first_commit = first_accept + warm_fragment + cold_extra;
        // The three uncovered successors walk while that first fragment is in
        // flight. Tile drain finishes on the final retirement's own edge.
        constexpr unsigned traversal_end = command_capture + edge_setup + candidates;
        constexpr unsigned draw_edges = first_commit > traversal_end ? first_commit : traversal_end;
        static_assert(warm_fragment == 41 && cold_extra == 21 && first_commit == 88 && draw_edges == 88);

        tile.begin(prepared(vertices, state), {0, 0});
        while (tile.busy()) {
            const auto elapsed = tile.statistics().cycles - before;
            expect(elapsed <= draw_edges, "one-sample draw exceeded its bounded clock schedule");
            if (elapsed < first_commit)
                expect(tile.statistics().color_writes == 0,
                       "fragment committed before registered child results reached the parent");
            tile.tick();
        }
        const auto& counts = tile.statistics();
        expect(counts.cycles - before == draw_edges && counts.covered_fragments == 1 &&
               counts.candidates == candidates && counts.color_writes == 1,
               "cross-block register boundaries were skipped or double-counted");
        expect(counts.perspective_cycles >= PerspectivePipeline::enabled_edge_latency &&
               counts.texture_cycles >= TextureUnit::pipeline_hit_edges + cold_extra,
               "unit-active counters omitted registered arithmetic or texture service");
        const auto image = tile.store();
        expect(image.color[0] == 0x07e0 && image.color[1] == 0x1234 &&
               image.depth[0] == depth_max, "timed one-sample draw produced incorrect attachments");
    });
    run_case("textured and 2D tile streams remain identical under common-clock stalls", [] {
        Pass pass;
        pass.clear_color = 0x2468;
        RenderState textured;
        textured.shade = ShadeMode::TextureGouraudModulate;
        textured.texture = pattern(16, 16, 40);
        textured.address_u = textured.address_v = AddressMode::Repeat;
        pass.primitives.emplace_back(prepared(textured_vertices(30, 30), textured));
        Rect rectangle;
        rectangle.bounds = {40, 40, 47, 48};
        rectangle.state = overlay_state({255, 80, 30});
        pass.primitives.emplace_back(rectangle);
        Sprite sprite;
        sprite.bounds = {42, 44, 50, 52};
        sprite.state.texture = pattern(8, 8);
        sprite.state.color_key = sprite.state.texture->at(0, 0);
        pass.primitives.emplace_back(sprite);
        Line line;
        line.a = {30, 60}; line.b = {67, 31};
        line.state = overlay_state({40, 230, 120});
        pass.primitives.emplace_back(line);

        Renderer canonical;
        canonical.render(pass);
        OpaqueTile normal, stalled;
        normal.clear(pass.clear_color, depth_max);
        stalled.clear(pass.clear_color, depth_max);
        std::uint64_t stalls = 0;
        for (const auto& primitive : pass.primitives) {
            normal.begin(primitive, {32, 32});
            stalled.begin(primitive, {32, 32});
            for (std::uint64_t clock = 0; normal.busy(); ++clock) {
                expect(clock < 2000000, "normal tile stream failed bounded progress");
                normal.tick();
            }
            for (std::uint64_t clock = 0; stalled.busy(); ++clock) {
                expect(clock < 3000000, "stalled tile stream failed bounded progress");
                const bool enabled = clock % 5 != 1 && clock % 7 != 3;
                stalled.tick(enabled);
                if (!enabled) ++stalls;
            }
        }
        const auto a = normal.store();
        const auto b = stalled.store();
        expect(a.color == b.color && a.depth == b.depth, "stalled pipeline duplicated/lost a fragment");
        for (int y = 0; y < tile_size; ++y) for (int x = 0; x < tile_size; ++x) {
            const auto local = static_cast<std::size_t>(y) * tile_size + x;
            expect(a.color[local] == canonical.color(1)[index(x + 32, y + 32)] &&
                   a.depth[local] == canonical.depth()[index(x + 32, y + 32)],
                   "direct textured tile differs from canonical attachments");
        }
        expect(stalls > 0 && stalled.statistics().stalls == stalls &&
               stalled.statistics().cycles == normal.statistics().cycles + stalls,
               "disabled clocks were not counted separately from enabled work");
    });
}

std::uint64_t color_hash(const std::vector<std::uint16_t>& pixels) {
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto pixel : pixels) {
        value ^= pixel & 255u; value *= 1099511628211ULL;
        value ^= pixel >> 8; value *= 1099511628211ULL;
    }
    return value;
}

void demo_tests() {
    for (const auto scene : {DemoScene::Showcase, DemoScene::Perspective}) {
        for (const long double time : {0.0L, 1.25L}) {
            run_case(std::string("identical ") + scene_name(scene) + " pass at time " + std::to_string(static_cast<double>(time)), [=] {
                // Generate exactly once. Both backends receive the same records,
                // textures, animation time, state, primitive order, and clear values.
                const Pass pass = make_demo_pass(scene, 1, time);
                const auto actual = compare_pass(pass);
                expect(actual.statistics().texture_samples != 0 && actual.statistics().keyed_rejected != 0,
                       "the original textured/keyed workload must actually execute");
                if (scene == DemoScene::Showcase && time == 0)
                    expect(color_hash(actual.color(1)) == 0x1d85d76382a08afbULL,
                           "original showcase golden framebuffer changed");
            });
        }
    }
    run_case("original overdraw workload remains identical", [] {
        compare_pass(make_demo_pass(DemoScene::Overdraw, 1, 1.25L));
    });
}

} // namespace

int main() {
    divider_tests();
    texture_tests();
    walker_tests();
    rendering_tests();
    copy_tests();
    tile_stall_tests();
    demo_tests();
    std::cout << cases_run - cases_failed << '/' << cases_run << " full hardware parity cases passed\n";
    return cases_failed ? 1 : 0;
}
