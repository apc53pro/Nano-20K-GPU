#include "nano_gpu/reference.hpp"
#ifndef NANO_GPU_ORACLE_ONLY
#include "nano_gpu/hw/tile_pipeline.hpp"
#include "nano_gpu/hw/blit.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace nano_gpu {
namespace {

constexpr std::size_t frame_pixels =
    static_cast<std::size_t>(screen_width) * screen_height;
constexpr std::size_t heap_bytes = 0x800000u - 0x258000u;

bool power_of_two(int value) {
    return value > 0 && (value & (value - 1)) == 0;
}

void check_buffer(int id) {
    if (id != 0 && id != 1) {
        throw std::out_of_range("color buffer ID must be 0 or 1");
    }
}

void check_address_mode(AddressMode mode) {
    if (mode != AddressMode::Clamp && mode != AddressMode::Repeat) {
        throw std::invalid_argument("unsupported texture address mode");
    }
}

bool textured(ShadeMode mode) {
    return mode == ShadeMode::TextureReplace ||
           mode == ShadeMode::TextureFlatModulate ||
           mode == ShadeMode::TextureGouraudModulate;
}

void check_state(const RenderState& state) {
    switch (state.depth_func) {
    case DepthFunc::Less:
    case DepthFunc::LessEqual:
    case DepthFunc::Always:
        break;
    default:
        throw std::invalid_argument("unsupported depth comparison");
    }
    switch (state.shade) {
    case ShadeMode::Flat:
    case ShadeMode::Gouraud:
    case ShadeMode::TextureReplace:
    case ShadeMode::TextureFlatModulate:
    case ShadeMode::TextureGouraudModulate:
        break;
    default:
        throw std::invalid_argument("unsupported shading mode");
    }
    check_address_mode(state.address_u);
    check_address_mode(state.address_v);
}

void check_texture(const RenderState& state) {
    if (!state.texture || !state.texture->texture_compatible()) {
        throw std::invalid_argument("texture requires a power-of-two RGB565 image");
    }
}

void check_constant_depth(std::uint32_t depth) {
    if (depth > depth_max) {
        throw std::invalid_argument("constant depth exceeds D24 range");
    }
}

void validate_primitive(const Primitive& primitive) {
    std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        check_state(item.state);
        if constexpr (std::is_same_v<T, Triangle>) {
            if (item.state.color_key && !textured(item.state.shade)) {
                throw std::invalid_argument("color key requires textured shading");
            }
            if (textured(item.state.shade) &&
                (item.state.color_write || item.state.color_key.has_value())) {
                check_texture(item.state);
            }
            validate_triangle(item);
        } else {
            check_constant_depth(item.depth);
            if constexpr (std::is_same_v<T, Sprite>) {
                if (item.state.shade != ShadeMode::Flat &&
                    item.state.shade != ShadeMode::TextureReplace) {
                    throw std::invalid_argument("sprite shading is raw replacement");
                }
                check_texture(item.state);
                const auto width = static_cast<std::int64_t>(item.bounds.x1) -
                                   item.bounds.x0;
                const auto height = static_cast<std::int64_t>(item.bounds.y1) -
                                    item.bounds.y0;
                if (item.source_x < 0 || item.source_y < 0) {
                    throw std::invalid_argument("negative sprite source origin");
                }
                if (!item.bounds.empty() &&
                    (static_cast<std::int64_t>(item.source_x) + width >
                         item.state.texture->width() ||
                     static_cast<std::int64_t>(item.source_y) + height >
                         item.state.texture->height())) {
                    throw std::invalid_argument("sprite source rectangle exceeds texture");
                }
            } else {
                if (item.state.shade != ShadeMode::Flat || item.state.color_key) {
                    throw std::invalid_argument("rectangle/line requires flat unkeyed color");
                }
                if constexpr (std::is_same_v<T, Line>) {
                    const Bounds viewport{};
                    if (!viewport.contains(item.a.x, item.a.y) ||
                        !viewport.contains(item.b.x, item.b.y)) {
                        throw std::invalid_argument("line endpoints must be viewport-clipped");
                    }
                }
            }
        }
    }, primitive);
}

bool depth_passes(const RenderState& state, std::uint32_t incoming,
                  std::uint32_t stored) {
    if (!state.depth_test) {
        return true;
    }
    switch (state.depth_func) {
    case DepthFunc::Less:
        return incoming < stored;
    case DepthFunc::LessEqual:
        return incoming <= stored;
    case DepthFunc::Always:
        return true;
    default:
        throw std::invalid_argument("unsupported depth comparison");
    }
}

template <typename ReadDepth, typename WriteDepth, typename WriteColor>
void fragment(const Primitive& primitive, int x, int y, Statistics& statistics,
              ReadDepth&& read_depth, WriteDepth&& write_depth,
              WriteColor&& write_color) {
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        const auto& state = item.state;
        ++statistics.covered_fragments;
        std::uint32_t incoming_depth = 0;
        if (state.depth_test || state.depth_write) {
            if constexpr (std::is_same_v<T, Triangle>) {
                incoming_depth = depth_code(item.z.at(x - item.bounds.x0,
                                                     y - item.bounds.y0));
            } else {
                incoming_depth = item.depth;
            }
        }
        if (!depth_passes(state, incoming_depth, read_depth())) {
            ++statistics.depth_rejected;
            return;
        }

        Color result = state.color;
        if constexpr (std::is_same_v<T, Triangle>) {
            const bool need_texture = textured(state.shade) &&
                (state.color_write || state.color_key.has_value());
            if (need_texture) {
                const Point uv = texture_coordinates(item, x, y);
                const auto texel = state.texture->sample(floor_q16(uv.x),
                                                        floor_q16(uv.y),
                                                        state.address_u,
                                                        state.address_v);
                ++statistics.texture_samples;
                if (state.color_key && texel == *state.color_key) {
                    ++statistics.keyed_rejected;
                    return;
                }
                if (state.color_write) {
                    result = expand_rgb565(texel);
                    if (state.shade == ShadeMode::TextureFlatModulate) {
                        result = modulate(result, state.color);
                    } else if (state.shade == ShadeMode::TextureGouraudModulate) {
                        const int dx = x - item.bounds.x0;
                        const int dy = y - item.bounds.y0;
                        result = modulate(result, Color{color_code(item.r.at(dx, dy)),
                                                        color_code(item.g.at(dx, dy)),
                                                        color_code(item.b.at(dx, dy))});
                    }
                }
            } else if (state.color_write && state.shade == ShadeMode::Gouraud) {
                const int dx = x - item.bounds.x0;
                const int dy = y - item.bounds.y0;
                result = Color{color_code(item.r.at(dx, dy)),
                               color_code(item.g.at(dx, dy)),
                               color_code(item.b.at(dx, dy))};
            }
        } else if constexpr (std::is_same_v<T, Sprite>) {
            if (state.color_write || state.color_key) {
                const auto sx = static_cast<std::int64_t>(item.source_x) + x -
                                item.bounds.x0;
                const auto sy = static_cast<std::int64_t>(item.source_y) + y -
                                item.bounds.y0;
                const auto texel = state.texture->at(static_cast<int>(sx),
                                                    static_cast<int>(sy));
                ++statistics.texture_samples;
                if (state.color_key && texel == *state.color_key) {
                    ++statistics.keyed_rejected;
                    return;
                }
                result = expand_rgb565(texel);
            }
        }
        // All inputs and rejection decisions precede either attachment write.
        if (state.color_write) {
            write_color(pack_rgb565(result));
            ++statistics.color_writes;
        }
        if (state.depth_write) {
            write_depth(incoming_depth);
            ++statistics.depth_writes;
        }
    }, primitive);
}

template <typename Emit>
void visit_samples(const Primitive& primitive, Bounds limit,
                   Statistics& statistics, Emit&& emit) {
    const Bounds bounds = intersect(primitive_bounds(primitive), limit);
    if (bounds.empty()) {
        return;
    }
    if (const auto* line = std::get_if<Line>(&primitive)) {
        for (const auto point : line_pixels(line->a, line->b)) {
            if (bounds.contains(point.x, point.y)) {
                ++statistics.candidates;
                emit(point.x, point.y);
            }
        }
        return;
    }
    const auto* triangle = std::get_if<Triangle>(&primitive);
    for (int y = bounds.y0; y < bounds.y1; ++y) {
        for (int x = bounds.x0; x < bounds.x1; ++x) {
            ++statistics.candidates;
            if (!triangle || covered(*triangle, x, y)) {
                emit(x, y);
            }
        }
    }
}

struct CopyWindow {
    std::int64_t x0;
    std::int64_t y0;
    std::int64_t x1;
    std::int64_t y1;
};

CopyWindow copy_window(int source_width, int source_height, Point source,
                       Point destination, int width, int height) {
    if (width < 0 || height < 0) {
        throw std::invalid_argument("negative copy dimensions");
    }
    return {
        std::max({std::int64_t{0}, -static_cast<std::int64_t>(source.x),
                  -static_cast<std::int64_t>(destination.x)}),
        std::max({std::int64_t{0}, -static_cast<std::int64_t>(source.y),
                  -static_cast<std::int64_t>(destination.y)}),
        std::min({static_cast<std::int64_t>(width),
                  static_cast<std::int64_t>(source_width) - source.x,
                  static_cast<std::int64_t>(screen_width) - destination.x}),
        std::min({static_cast<std::int64_t>(height),
                  static_cast<std::int64_t>(source_height) - source.y,
                  static_cast<std::int64_t>(screen_height) - destination.y})
    };
}

bool full_destination(const CopyWindow& window, Point destination) {
    return window.x0 < window.x1 && window.y0 < window.y1 &&
           static_cast<std::int64_t>(destination.x) + window.x0 == 0 &&
           static_cast<std::int64_t>(destination.y) + window.y0 == 0 &&
           static_cast<std::int64_t>(destination.x) + window.x1 == screen_width &&
           static_cast<std::int64_t>(destination.y) + window.y1 == screen_height;
}

} // namespace

Color expand_rgb565(std::uint16_t value) {
    const auto r = static_cast<std::uint8_t>((value >> 11) & 31);
    const auto g = static_cast<std::uint8_t>((value >> 5) & 63);
    const auto b = static_cast<std::uint8_t>(value & 31);
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)),
            static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2))};
}

std::uint16_t pack_rgb565(Color color) {
    return static_cast<std::uint16_t>(((color.r >> 3) << 11) |
                                      ((color.g >> 2) << 5) |
                                      (color.b >> 3));
}

Color modulate(Color texture, Color color) {
    return {static_cast<std::uint8_t>((texture.r * color.r + 127) / 255),
            static_cast<std::uint8_t>((texture.g * color.g + 127) / 255),
            static_cast<std::uint8_t>((texture.b * color.b + 127) / 255)};
}

Image::Image(int width, int height, int pitch_bytes)
    : Image(width, height, pitch_bytes, true) {}

Image::Image(int width, int height, int pitch_bytes, bool allocate_pixels)
    : width_(width), height_(height), pitch_bytes_(pitch_bytes) {
    if (width < 1 || width > 1024 || height < 1 || height > 1024) {
        throw std::invalid_argument("image dimensions must lie in 1..1024");
    }
    if (pitch_bytes_ == 0) {
        pitch_bytes_ = (2 * width + 3) & ~3;
    }
    if (pitch_bytes_ < 2 * width || pitch_bytes_ % 4 != 0) {
        throw std::invalid_argument("image pitch must be aligned and cover each row");
    }
    const auto extent = static_cast<std::uint64_t>(pitch_bytes_) * height;
    if (extent > heap_bytes) {
        throw std::invalid_argument("image extent exceeds the GPU heap");
    }
    if (allocate_pixels) pixels_.assign(static_cast<std::size_t>(extent / 2), 0);
}

std::shared_ptr<const Image> Image::descriptor(int width, int height, int pitch_bytes) {
    return std::shared_ptr<const Image>(new Image(width, height, pitch_bytes, false));
}

std::uint16_t& Image::at(int x, int y) {
    if (pixels_.empty()) throw std::logic_error("metadata-only image has no host pixel port");
    if (x < 0 || x >= width_ || y < 0 || y >= height_) {
        throw std::out_of_range("image pixel outside image bounds");
    }
    return pixels_[static_cast<std::size_t>(y) * (pitch_bytes_ / 2) + x];
}

std::uint16_t Image::at(int x, int y) const {
    if (pixels_.empty()) throw std::logic_error("metadata-only image has no host pixel port");
    if (x < 0 || x >= width_ || y < 0 || y >= height_) {
        throw std::out_of_range("image pixel outside image bounds");
    }
    return pixels_[static_cast<std::size_t>(y) * (pitch_bytes_ / 2) + x];
}

std::uint16_t Image::sample(int x, int y, AddressMode u, AddressMode v) const {
    check_address_mode(u);
    check_address_mode(v);
    const auto address = [](int value, int size, AddressMode mode) {
        if (mode == AddressMode::Clamp) {
            return std::clamp(value, 0, size - 1);
        }
        const int residue = value % size;
        return residue < 0 ? residue + size : residue;
    };
    return at(address(x, width_, u), address(y, height_, v));
}

bool Image::texture_compatible() const {
    return power_of_two(width_) && power_of_two(height_);
}

RenderState overlay_state(Color color) {
    RenderState state;
    state.depth_test = false;
    state.depth_write = false;
    state.color = color;
    return state;
}

Bounds primitive_bounds(const Primitive& primitive) {
    return std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        Bounds bounds;
        if constexpr (std::is_same_v<T, Line>) {
            const Bounds viewport{};
            if (!viewport.contains(item.a.x, item.a.y) ||
                !viewport.contains(item.b.x, item.b.y)) {
                throw std::invalid_argument("line endpoints must be viewport-clipped");
            }
            bounds = {std::min(item.a.x, item.b.x), std::min(item.a.y, item.b.y),
                      std::max(item.a.x, item.b.x) + 1,
                      std::max(item.a.y, item.b.y) + 1};
        } else {
            bounds = item.bounds;
        }
        return intersect(intersect(bounds, item.state.scissor), Bounds{});
    }, primitive);
}

TileBins bin_primitives(const std::vector<Primitive>& primitives) {
    TileBins bins;
    for (std::size_t index = 0; index < primitives.size(); ++index) {
        const Bounds bounds = primitive_bounds(primitives[index]);
        if (bounds.empty()) {
            continue;
        }
        for (int ty = bounds.y0 / tile_size; ty <= (bounds.y1 - 1) / tile_size; ++ty) {
            for (int tx = bounds.x0 / tile_size; tx <= (bounds.x1 - 1) / tile_size; ++tx) {
                bins[static_cast<std::size_t>(ty * tile_columns + tx)].push_back(index);
            }
        }
    }
    return bins;
}

Renderer::Renderer() { reset(); }

void Renderer::reset() {
    for (auto& buffer : color_) {
        buffer.assign(frame_pixels, 0);
    }
    depth_.assign(frame_pixels, depth_max);
    color_valid_ = {{true, true}};
    depth_valid_ = true;
    front_ = 0;
    pending_.reset();
    statistics_ = {};
}

void Renderer::require_writable(int target) const {
    check_buffer(target);
    if (target == front_) {
        throw std::invalid_argument("displayed color buffer is not writable");
    }
    if (pending_) {
        throw std::invalid_argument("presentation seals the back buffer until vertical blank");
    }
}

void Renderer::render(const Pass& pass, RenderMode mode) {
    require_writable(pass.target);
    if (mode != RenderMode::Tiled && mode != RenderMode::TiledReverse &&
        mode != RenderMode::Monolithic && mode != RenderMode::HardwareBlocks) {
        throw std::invalid_argument("unsupported render mode");
    }
    if ((pass.color_load != LoadOp::Load && pass.color_load != LoadOp::Clear) ||
        (pass.depth_load != LoadOp::Load && pass.depth_load != LoadOp::Clear)) {
        throw std::invalid_argument("unsupported attachment load operation");
    }
    check_constant_depth(pass.clear_depth);
    if ((pass.color_load == LoadOp::Load && !color_valid_[pass.target]) ||
        (pass.depth_load == LoadOp::Load && !depth_valid_)) {
        throw std::invalid_argument("LOAD requires initialized attachment contents");
    }
    statistics_ = {};
    try {
        for (const auto& primitive : pass.primitives) {
            validate_primitive(primitive);
        }
        if (mode == RenderMode::HardwareBlocks) {
#ifdef NANO_GPU_ORACLE_ONLY
            throw std::invalid_argument("the RTL oracle build cannot invoke a C++ hardware backend");
#else
            hw::render_pass(pass, color_[pass.target], depth_, statistics_);
            color_valid_[pass.target] = true;
            depth_valid_ = true;
            return;
#endif
        }
        const TileBins bins = bin_primitives(pass.primitives);
        for (const auto& bin : bins) {
            statistics_.tile_references += bin.size();
        }
        auto& output = color_[pass.target];
        if (mode == RenderMode::Monolithic) {
            statistics_.tiles_processed = 1;
            if (pass.color_load == LoadOp::Clear) {
                std::fill(output.begin(), output.end(), pass.clear_color);
            }
            if (pass.depth_load == LoadOp::Clear) {
                std::fill(depth_.begin(), depth_.end(), pass.clear_depth);
            }
            for (const auto& primitive : pass.primitives) {
                visit_samples(primitive, Bounds{}, statistics_, [&](int x, int y) {
                    const auto index = static_cast<std::size_t>(y) * screen_width + x;
                    fragment(primitive, x, y, statistics_,
                             [&] { return depth_[index]; },
                             [&](std::uint32_t value) { depth_[index] = value; },
                             [&](std::uint16_t value) { output[index] = value; });
                });
            }
        } else {
            // These are the actual tile-local attachments, not aliases of the frame.
            std::array<std::uint16_t, tile_size * tile_size> tile_color{};
            std::array<std::uint32_t, tile_size * tile_size> tile_depth{};
            for (int visit = 0; visit < tile_count; ++visit) {
                const int tile = mode == RenderMode::TiledReverse
                    ? tile_count - 1 - visit : visit;
                const int x0 = (tile % tile_columns) * tile_size;
                const int y0 = (tile / tile_columns) * tile_size;
                for (int ly = 0; ly < tile_size; ++ly) {
                    for (int lx = 0; lx < tile_size; ++lx) {
                        const auto local = static_cast<std::size_t>(ly * tile_size + lx);
                        const auto global = static_cast<std::size_t>(y0 + ly) *
                                            screen_width + x0 + lx;
                        tile_color[local] = pass.color_load == LoadOp::Clear
                            ? pass.clear_color : output[global];
                        tile_depth[local] = pass.depth_load == LoadOp::Clear
                            ? pass.clear_depth : depth_[global];
                    }
                }
                ++statistics_.tiles_processed;
                const Bounds tile_bounds{x0, y0, x0 + tile_size, y0 + tile_size};
                for (const auto reference : bins[static_cast<std::size_t>(tile)]) {
                    const auto& primitive = pass.primitives[reference];
                    visit_samples(primitive, tile_bounds, statistics_, [&](int x, int y) {
                        const auto local = static_cast<std::size_t>(y - y0) * tile_size +
                                           x - x0;
                        fragment(primitive, x, y, statistics_,
                                 [&] { return tile_depth[local]; },
                                 [&](std::uint32_t value) { tile_depth[local] = value; },
                                 [&](std::uint16_t value) { tile_color[local] = value; });
                    });
                }
                for (int ly = 0; ly < tile_size; ++ly) {
                    for (int lx = 0; lx < tile_size; ++lx) {
                        const auto local = static_cast<std::size_t>(ly * tile_size + lx);
                        const auto global = static_cast<std::size_t>(y0 + ly) *
                                            screen_width + x0 + lx;
                        output[global] = tile_color[local];
                        depth_[global] = tile_depth[local];
                    }
                }
            }
        }
        color_valid_[pass.target] = true;
        depth_valid_ = true;
    } catch (...) {
        // A pass is not transactional. Do not permit stale validity after failure.
        color_valid_[pass.target] = false;
        depth_valid_ = false;
        throw;
    }
}

void Renderer::copy_rect(int source, int target, Point source_origin,
                         Point destination_origin, int width, int height, RenderMode mode) {
    if (mode != RenderMode::Tiled && mode != RenderMode::TiledReverse &&
        mode != RenderMode::Monolithic && mode != RenderMode::HardwareBlocks)
        throw std::invalid_argument("unsupported copy mode");
    check_buffer(source);
    require_writable(target);
    if (!color_valid_[source]) {
        throw std::invalid_argument("copy source must have initialized color");
    }
    const auto window = copy_window(screen_width, screen_height, source_origin,
                                    destination_origin, width, height);
    statistics_.hardware_copy_cycles = 0;
    statistics_.hardware_copy_words = 0;
    if (window.x0 >= window.x1 || window.y0 >= window.y1) {
        return;
    }
    if (mode == RenderMode::HardwareBlocks) {
#ifdef NANO_GPU_ORACLE_ONLY
        throw std::invalid_argument("the RTL oracle build cannot invoke a C++ copy backend");
#else
        const hw::CopyRegion region{
            {static_cast<int>(source_origin.x + window.x0), static_cast<int>(source_origin.y + window.y0)},
            {static_cast<int>(destination_origin.x + window.x0), static_cast<int>(destination_origin.y + window.y0)},
            static_cast<int>(window.x1 - window.x0), static_cast<int>(window.y1 - window.y0)};
        const auto counters = hw::copy_framebuffer(color_[source], color_[target], region);
        statistics_.hardware_copy_cycles = counters.cycles;
        statistics_.hardware_copy_words = counters.words;
        if (full_destination(window, destination_origin)) color_valid_[target] = true;
        return;
#endif
    }
    // A host-side snapshot directly defines the specified overlapping-copy result.
    std::vector<std::uint16_t> snapshot;
    snapshot.reserve(static_cast<std::size_t>(window.x1 - window.x0) *
                     static_cast<std::size_t>(window.y1 - window.y0));
    for (auto j = window.y0; j < window.y1; ++j) {
        for (auto i = window.x0; i < window.x1; ++i) {
            const auto index = static_cast<std::size_t>(source_origin.y + j) *
                               screen_width + static_cast<std::size_t>(source_origin.x + i);
            snapshot.push_back(color_[source][index]);
        }
    }
    std::size_t sample = 0;
    for (auto j = window.y0; j < window.y1; ++j) {
        for (auto i = window.x0; i < window.x1; ++i) {
            const auto index = static_cast<std::size_t>(destination_origin.y + j) *
                               screen_width + static_cast<std::size_t>(destination_origin.x + i);
            color_[target][index] = snapshot[sample++];
        }
    }
    if (full_destination(window, destination_origin)) {
        color_valid_[target] = true;
    }
}

void Renderer::copy_image(const Image& source, int target, Point source_origin,
                          Point destination_origin, int width, int height, RenderMode mode) {
    if (mode != RenderMode::Tiled && mode != RenderMode::TiledReverse &&
        mode != RenderMode::Monolithic && mode != RenderMode::HardwareBlocks)
        throw std::invalid_argument("unsupported copy mode");
    require_writable(target);
    const auto window = copy_window(source.width(), source.height(), source_origin,
                                    destination_origin, width, height);
    statistics_.hardware_copy_cycles = 0;
    statistics_.hardware_copy_words = 0;
    if (window.x0 >= window.x1 || window.y0 >= window.y1) {
        return;
    }
    if (mode == RenderMode::HardwareBlocks) {
#ifdef NANO_GPU_ORACLE_ONLY
        throw std::invalid_argument("the RTL oracle build cannot invoke a C++ copy backend");
#else
        const hw::CopyRegion region{
            {static_cast<int>(source_origin.x + window.x0), static_cast<int>(source_origin.y + window.y0)},
            {static_cast<int>(destination_origin.x + window.x0), static_cast<int>(destination_origin.y + window.y0)},
            static_cast<int>(window.x1 - window.x0), static_cast<int>(window.y1 - window.y0)};
        const auto counters = hw::copy_from_image(source, color_[target], region);
        statistics_.hardware_copy_cycles = counters.cycles;
        statistics_.hardware_copy_words = counters.words;
        if (full_destination(window, destination_origin)) color_valid_[target] = true;
        return;
#endif
    }
    for (auto j = window.y0; j < window.y1; ++j) {
        for (auto i = window.x0; i < window.x1; ++i) {
            const auto index = static_cast<std::size_t>(destination_origin.y + j) *
                               screen_width + static_cast<std::size_t>(destination_origin.x + i);
            color_[target][index] = source.at(static_cast<int>(source_origin.x + i),
                                              static_cast<int>(source_origin.y + j));
        }
    }
    if (full_destination(window, destination_origin)) {
        color_valid_[target] = true;
    }
}

void Renderer::queue_present(int target) {
    require_writable(target);
    if (!color_valid_[target]) {
        throw std::invalid_argument("presentation requires initialized color");
    }
    pending_ = target;
}

void Renderer::vertical_blank() {
    if (pending_) {
        front_ = *pending_;
        pending_.reset();
    }
}

void Renderer::abort() {
    // Calls execute synchronously, so completed attachments are not interrupted.
    pending_.reset();
}

bool Renderer::color_valid(int id) const {
    check_buffer(id);
    return color_valid_[id];
}

const std::vector<std::uint16_t>& Renderer::color(int id) const {
    check_buffer(id);
    return color_[id];
}

} // namespace nano_gpu
