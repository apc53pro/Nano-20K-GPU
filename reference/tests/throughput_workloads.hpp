#pragma once

#include "nano_gpu/hw/throughput.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace nano_gpu::throughput_workloads {

inline std::shared_ptr<Image> image(unsigned size, bool keyed = false) {
    auto texture = std::make_shared<Image>(static_cast<int>(size), static_cast<int>(size));
    for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x)
            texture->at(static_cast<int>(x), static_cast<int>(y)) = keyed ? 0x4321u :
                static_cast<std::uint16_t>(((x * 3 + y) & 31u) << 11 |
                    ((x + y * 5) & 63u) << 5 | ((x * 7 + y * 3) & 31u));
    return texture;
}

inline RenderState depth_state() {
    RenderState state;
    state.color = {239, 101, 47};
    state.depth_test = true; state.depth_write = true;
    return state;
}

inline Triangle triangle(bool textured) {
    auto state = depth_state();
    state.shade = textured ? ShadeMode::TextureGouraudModulate : ShadeMode::Gouraud;
    if (textured) state.texture = image(4);
    std::array<ScreenVertex, 3> vertices{{{0, 0, 0.2L, 1, 0, 0, 255, 32, 64},
                                       {64, 0, 0.4L, 2, 4, 0, 32, 255, 64},
                                       {0, 64, 0.6L, 4, 0, 4, 64, 32, 255}}};
    auto prepared = setup_triangle(vertices, state);
    if (!prepared) throw std::runtime_error("throughput triangle unexpectedly culled");
    return *prepared;
}

inline Primitive primitive(const std::string& name) {
    if (name == "flat" || name == "depth-reject") {
        Rect rect;
        rect.bounds = {0, 0, tile_size, tile_size};
        rect.state = depth_state();
        rect.depth = name == "depth-reject" ? depth_max : 0x123456;
        return rect;
    }
    if (name == "gouraud" || name == "textured" || name == "textured-hit")
        return triangle(name != "gouraud");
    if (name == "sprite" || name == "sprite-hit" || name == "key-reject") {
        Sprite sprite;
        sprite.bounds = {0, 0, tile_size, tile_size};
        sprite.state = depth_state();
        sprite.state.texture = image(tile_size, name == "key-reject");
        sprite.depth = 0x123456;
        if (name == "key-reject") sprite.state.color_key = 0x4321;
        return sprite;
    }
    throw std::invalid_argument("unknown throughput workload: " + name);
}

inline const std::array<const char*, 8>& names() {
    static constexpr std::array<const char*, 8> values{{
        "flat", "gouraud", "textured", "textured-hit", "sprite", "sprite-hit",
        "depth-reject", "key-reject"}};
    return values;
}

inline bool warm_hit(const std::string& name) {
    return name == "textured-hit" || name == "sprite-hit";
}

inline void draw(hw::OpaqueTile& tile, const Primitive& primitive, bool stalls = false) {
    tile.begin(primitive, {0, 0});
    std::uint64_t edge = 0;
    while (tile.busy()) {
        if (++edge > 4000000) throw std::runtime_error("throughput workload failed to drain");
        tile.tick(!stalls || (edge % 5 != 1 && edge % 7 != 3));
    }
}

inline void warm_cache(hw::OpaqueTile& tile, const Primitive& primitive) {
    if (const auto* triangle = std::get_if<Triangle>(&primitive)) {
        // A one-pixel sprite binds the same immutable image and fills its first
        // cache line without modifying CPU-prepared triangle bounds/anchors.
        Sprite warm;
        warm.bounds = {0, 0, 1, 1}; warm.state = triangle->state;
        warm.state.depth_test = warm.state.depth_write = false;
        warm.state.color_write = true; warm.state.shade = ShadeMode::TextureReplace;
        draw(tile, warm);
        return;
    }
    auto warm = primitive;
    std::visit([](auto& item) {
        item.state.depth_test = item.state.depth_write = false;
    }, warm);
    draw(tile, warm);
}

} // namespace nano_gpu::throughput_workloads
