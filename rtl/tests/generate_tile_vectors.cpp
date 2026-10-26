// Generate independent pixel expectations with the canonical monolithic SRR.
// The RTL receives only packed primitive records and texture-interface data;
// no software hardware-block renderer participates in the comparison.
#include "nano_gpu/reference.hpp"
#include "nano_gpu/hw/protocol.hpp"

#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace nano_gpu;

namespace {
struct TestCase {
    Point tile;
    Pass pass;
};

void generate(const char* path) {
    auto texture = std::make_shared<Image>(8, 8, 16);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            texture->at(x, y) = (x + 2 * y) % 7 == 0 ? 0 :
                static_cast<std::uint16_t>(0x421 + 0x181 * x + 0x423 * y);
        }
    }
    std::vector<TestCase> cases;
    for (int mode = 0; mode < 5; ++mode) {
        for (int origin : {0, 32}) {
            Pass pass;
            pass.clear_color = 0x1342;
            pass.clear_depth = 0xabcdef;
            RenderState state;
            state.shade = static_cast<ShadeMode>(mode);
            state.color = {180, 90, 220};
            state.depth_func = DepthFunc::LessEqual;
            state.address_u = AddressMode::Repeat;
            state.address_v = AddressMode::Clamp;
            if (mode >= 2) {
                state.texture = texture;
                state.color_key = 0;
            }
            const std::array<ScreenVertex, 3> vertices{{
                {4, 3, 0.25L, 1, -1.25L, 0.5L, 240, 20, 80},
                {70, 8, 0.65L, 2, 15.75L, -2, 20, 240, 80},
                {10, 70, 0.45L, 4, -1, 16.75L, 80, 20, 240}}};
            const auto triangle = setup_triangle(vertices, state);
            if (!triangle) throw std::runtime_error("oracle triangle setup failed");
            pass.primitives.emplace_back(*triangle);
            cases.push_back({{origin, origin}, std::move(pass)});
        }
    }
    for (auto mode : {DepthFunc::Less, DepthFunc::LessEqual, DepthFunc::Always}) {
        Pass pass;
        pass.clear_color = 0x4321;
        pass.clear_depth = 0x654321;
        auto state = overlay_state({251, 37, 149});
        state.depth_test = true;
        state.depth_write = true;
        state.depth_func = mode;
        pass.primitives.emplace_back(Rect{{-3, -2, 37, 34}, state, 0x654321});
        state.color = {13, 247, 52};
        pass.primitives.emplace_back(Rect{{7, 9, 22, 24}, state, 0x123456});
        cases.push_back({{0, 0}, std::move(pass)});
    }
    for (bool reverse : {false, true}) {
        for (int slope = 0; slope < 4; ++slope) {
            Pass pass;
            pass.clear_color = 0x5555;
            pass.clear_depth = 0xffffff;
            auto state = overlay_state({221, 53, 173});
            state.scissor = {3, 2, 60, 60};
            Point a{0, 0}, b{63, 31};
            if (slope == 1) { a = {0, 63}; b = {31, 0}; }
            if (slope == 2) { a = {31, 0}; b = {0, 63}; }
            if (slope == 3) { a = {20, 0}; b = {20, 63}; }
            if (reverse) std::swap(a, b);
            pass.primitives.emplace_back(Line{a, b, state, 0});
            cases.push_back({{0, 0}, std::move(pass)});
        }
    }
    for (int origin : {0, 32}) {
        for (bool key : {false, true}) {
            Pass pass;
            pass.clear_color = 0x6b4d;
            pass.clear_depth = 0xfedcba;
            auto state = overlay_state();
            state.texture = texture;
            if (key) state.color_key = 0;
            state.depth_test = true;
            state.depth_write = true;
            const int x = origin - 2, y = origin - 1;
            pass.primitives.emplace_back(Sprite{{x, y, x + 8, y + 8}, 0, 0, state, 0x987654});
            cases.push_back({{origin, origin}, std::move(pass)});
        }
    }

    // Exercise both coordinate axes above their half-range in the last tile.
    // This catches accidental signed truncation at X=512 or Y=256, while all
    // triangle coefficients still come from the independent CPU setup path.
    for (int mode = 0; mode < 5; ++mode) {
        for (const auto tile : {Point{576, 416}, Point{608, 448}}) {
            Pass pass; pass.clear_color = 0x2953; pass.clear_depth = 0xcdef01;
            RenderState state;
            state.shade = static_cast<ShadeMode>(mode);
            state.color = {201, 67, 159};
            state.depth_func = DepthFunc::LessEqual;
            state.address_u = AddressMode::Repeat;
            state.address_v = AddressMode::Clamp;
            if (mode >= 2) { state.texture = texture; state.color_key = 0; }
            const std::array<ScreenVertex, 3> vertices{{
                {580, 418, 0.21L, 1, -1.25L, 0.5L, 240, 20, 80},
                {640, 425, 0.65L, 2, 15.75L, -2, 20, 240, 80},
                {602, 480, 0.45L, 4, -1, 16.75L, 80, 20, 240}}};
            const auto triangle = setup_triangle(vertices, state);
            if (!triangle) throw std::runtime_error("high-viewport triangle setup failed");
            pass.primitives.emplace_back(*triangle);
            cases.push_back({tile, std::move(pass)});
        }
    }

    const auto int_min = std::numeric_limits<std::int32_t>::min();
    const auto int_max = std::numeric_limits<std::int32_t>::max();
    const Bounds huge{int_min, int_min, int_max, int_max};
    const auto append = [&](Point tile, Primitive primitive) {
        Pass pass; pass.clear_color = 0x294b; pass.clear_depth = 0xbadcfe;
        pass.primitives.emplace_back(std::move(primitive));
        cases.push_back({tile, std::move(pass)});
    };
    // Bounds and scissors are signed32 input fields even though every retained
    // nonempty raster window is bounded by the 640x480 viewport. Keep empty
    // extreme/inverted fields in the stream so saturation cannot invent work.
    for (const auto tile : {Point{0, 0}, Point{608, 448}}) {
        const std::vector<std::pair<Bounds, Bounds>> windows{
            {huge, huge},
            {{int_min, int_min, tile.x + 5, tile.y + 7}, huge},
            {{tile.x + 31, tile.y + 31, int_max, int_max}, huge},
            {{tile.x - 5, tile.y - 5, tile.x + 33, tile.y + 33},
             {tile.x + 7, tile.y + 11, int_max, int_max}},
            {huge, {tile.x + 31, tile.y + 31, int_max, int_max}},
            {{int_max, int_min, int_max, int_max}, huge},
            {{int_min, int_max, int_max, int_max}, huge},
            {{int_max, int_max, int_min, int_min}, huge},
            {huge, {int_max, int_min, int_max, int_max}},
            {{int_min, int_min, -1, -1}, huge},
            {{641, 481, int_max, int_max}, huge},
            {huge, {int_min, int_min, -1, -1}},
            {huge, {int_max, int_max, int_min, int_min}},
        };
        for (const auto& window : windows) {
            auto state = overlay_state({211, 39, 147});
            state.scissor = window.second; state.depth_write = true;
            append(tile, Rect{window.first, state, 0x123456});
        }
    }
    for (const auto& endpoints : {
            std::pair<Point, Point>{{579, 415}, {639, 479}},
            {{639, 417}, {579, 479}}, {{639, 479}, {608, 448}},
            {{639, 448}, {639, 479}}, {{608, 479}, {639, 479}},
            {{639, 479}, {639, 479}}}) {
        auto state = overlay_state({37, 211, 89});
        state.scissor = huge; state.depth_write = true;
        append({608, 448}, Line{endpoints.first, endpoints.second, state, 0x765432});
    }
    for (bool key : {false, true}) {
        const std::vector<std::pair<Point, Bounds>> sprite_windows{
            {{0, 0}, {-4, -3, 4, 5}},
            {{608, 448}, {636, 476, 644, 484}},
            {{608, 448}, {639, 479, 643, 483}},
            {{608, 448}, {608, 448, 612, 452}},
            {{0, 0}, {int_min, int_min, int_min + 8, int_min + 8}},
            {{608, 448}, {int_max - 8, int_max - 8, int_max, int_max}},
            {{0, 0}, {int_min, int_max, int_max, int_min}},
            {{608, 448}, {int_max, int_min, int_min, int_max}},
        };
        for (const auto& window : sprite_windows) {
            auto state = overlay_state();
            state.texture = texture; state.scissor = huge;
            state.depth_test = true; state.depth_write = true;
            if (key) state.color_key = 0;
            append(window.first, Sprite{window.second, 0, 0, state, 0x345678});
        }
    }

    std::ofstream output;
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.open(path);
    output << cases.size() << '\n';
    for (const auto& test : cases) {
        Renderer oracle;
        oracle.render(test.pass, RenderMode::Monolithic);
        output << std::dec << test.tile.x << ' ' << test.tile.y << ' '
               << std::hex << test.pass.clear_color << ' ' << test.pass.clear_depth
               << ' ' << std::dec << test.pass.primitives.size() << '\n';
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) output << std::hex << texture->at(x, y) << ' ';
        }
        output << '\n';
        for (const auto& primitive : test.pass.primitives) {
            const bool has_texture = std::visit([](const auto& item) {
                return static_cast<bool>(item.state.texture);
            }, primitive);
            const auto descriptor = has_texture ?
                std::optional<hw::TextureDescriptor>{{hw::heap_base, 8, 8, 16}} : std::nullopt;
            const auto record = hw::encode_primitive(primitive, descriptor);
            for (unsigned word = 0; word < 80; ++word) {
                output << std::hex << hw::read_u32(record, 4 * word) << ' ';
            }
            output << '\n';
        }
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 32; ++x) {
                const int index = (test.tile.y + y) * screen_width + test.tile.x + x;
                output << std::hex << oracle.color(test.pass.target)[index] << ' '
                       << oracle.depth()[index] << ' ';
            }
        }
        output << '\n';
    }
    output.close();
    std::cout << cases.size() << " canonical oracle tile cases\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: generate-tile-vectors OUTPUT\n";
        return 2;
    }
    try {
        generate(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << "generate-tile-vectors: " << error.what() << '\n';
        return 1;
    }
}
