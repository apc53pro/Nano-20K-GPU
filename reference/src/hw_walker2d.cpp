#include "nano_gpu/hw/walker2d.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace nano_gpu::hw {
namespace {

Bounds clipped(Bounds a, Bounds b) {
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0),
            std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

void validate_origin(Point origin) {
    if (origin.x < 0 || origin.x >= screen_width || origin.x % tile_size != 0 ||
        origin.y < 0 || origin.y >= screen_height || origin.y % tile_size != 0)
        throw std::invalid_argument("2D tile origin must name a visible 32x32 tile");
}

void validate_state(const RenderState& state) {
    switch (state.depth_func) {
    case DepthFunc::Less:
    case DepthFunc::LessEqual:
    case DepthFunc::Always:
        break;
    default:
        throw std::invalid_argument("unsupported 2D depth comparison");
    }
    switch (state.shade) {
    case ShadeMode::Flat:
    case ShadeMode::Gouraud:
    case ShadeMode::TextureReplace:
    case ShadeMode::TextureFlatModulate:
    case ShadeMode::TextureGouraudModulate:
        break;
    default:
        throw std::invalid_argument("unsupported 2D shading mode");
    }
    const auto address_mode = [](AddressMode mode) {
        if (mode != AddressMode::Clamp && mode != AddressMode::Repeat)
            throw std::invalid_argument("unsupported 2D texture address mode");
    };
    address_mode(state.address_u);
    address_mode(state.address_v);
}

} // namespace

void Walker2D::begin(const Primitive& primitive, Point tile_origin) {
    validate_origin(tile_origin);
    // Build a replacement register set. Every failure leaves the current
    // command intact, including a partially advanced line or sprite.
    Walker2D next;
    const Bounds tile{tile_origin.x, tile_origin.y,
                      tile_origin.x + tile_size, tile_origin.y + tile_size};
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, Triangle>) {
            throw std::invalid_argument("triangle records do not use the 2D walker");
        } else {
            validate_state(item.state);
            if (item.depth > depth_max)
                throw std::invalid_argument("2D constant depth exceeds D24");

            Bounds bounds;
            if constexpr (std::is_same_v<T, Sprite>) {
                if (item.state.shade != ShadeMode::Flat &&
                    item.state.shade != ShadeMode::TextureReplace)
                    throw std::invalid_argument("sprite shading is raw replacement");
                if (!item.state.texture || !item.state.texture->texture_compatible())
                    throw std::invalid_argument("sprite requires a power-of-two RGB565 image");
                if (item.source_x < 0 || item.source_y < 0)
                    throw std::invalid_argument("negative sprite source origin");
                // These wider host checks establish that a visible source
                // counter will fit ten bits before any subtraction/narrowing.
                const auto width = static_cast<std::int64_t>(item.bounds.x1) - item.bounds.x0;
                const auto height = static_cast<std::int64_t>(item.bounds.y1) - item.bounds.y0;
                if (!item.bounds.empty() &&
                    (static_cast<std::int64_t>(item.source_x) + width > item.state.texture->width() ||
                     static_cast<std::int64_t>(item.source_y) + height > item.state.texture->height()))
                    throw std::invalid_argument("sprite source rectangle exceeds texture");
                next.kind_ = Kind::Sprite;
                bounds = item.bounds;
            } else {
                if (item.state.shade != ShadeMode::Flat || item.state.color_key)
                    throw std::invalid_argument("rectangle/line requires flat unkeyed color");
                if constexpr (std::is_same_v<T, Line>) {
                    const Bounds viewport{};
                    if (!viewport.contains(item.a.x, item.a.y) ||
                        !viewport.contains(item.b.x, item.b.y))
                        throw std::invalid_argument("line endpoints must be viewport-clipped");
                    Point a = item.a;
                    Point b = item.b;
                    if (std::tie(b.x, b.y) < std::tie(a.x, a.y)) std::swap(a, b);
                    next.kind_ = Kind::Line;
                    next.x_ = a.x;
                    next.y_ = a.y;
                    next.end_x_ = b.x;
                    next.end_y_ = b.y;
                    // Endpoint bounds guarantee all differences and error
                    // updates fit signed 16 bits. No multiplied coordinates.
                    next.dx_ = static_cast<std::int16_t>(b.x >= a.x ? b.x - a.x : a.x - b.x);
                    next.dy_ = static_cast<std::int16_t>(-(b.y >= a.y ? b.y - a.y : a.y - b.y));
                    next.sx_ = static_cast<std::int16_t>(a.x < b.x ? 1 : -1);
                    next.sy_ = static_cast<std::int16_t>(a.y < b.y ? 1 : -1);
                    next.error_ = static_cast<std::int16_t>(next.dx_ + next.dy_);
                    bounds = {std::min(a.x, b.x), std::min(a.y, b.y),
                              std::max(a.x, b.x) + 1, std::max(a.y, b.y) + 1};
                } else {
                    next.kind_ = Kind::Grid;
                    bounds = item.bounds;
                }
            }
            next.window_ = clipped(clipped(clipped(bounds, item.state.scissor), tile), Bounds{});
            next.active_ = !next.window_.empty();
            if (next.kind_ != Kind::Line) {
                next.x_ = next.window_.x0;
                next.y_ = next.window_.y0;
            }
            if constexpr (std::is_same_v<T, Sprite>) {
                if (next.active_) {
                    const auto source_x = static_cast<std::int64_t>(item.source_x) + next.x_ - item.bounds.x0;
                    const auto source_y = static_cast<std::int64_t>(item.source_y) + next.y_ - item.bounds.y0;
                    if (source_x < 0 || source_x >= item.state.texture->width() ||
                        source_y < 0 || source_y >= item.state.texture->height())
                        throw std::invalid_argument("clipped sprite source is outside texture");
                    next.source_start_x_ = next.source_x_ = static_cast<std::int32_t>(source_x);
                    next.source_y_ = static_cast<std::int32_t>(source_y);
                }
            }
        }
    }, primitive);
    *this = next;
}

Sample2D Walker2D::sample() const {
    return {x_, y_, active_ && window_.contains(x_, y_),
            kind_ == Kind::Sprite ? source_x_ : 0,
            kind_ == Kind::Sprite ? source_y_ : 0};
}

void Walker2D::tick(bool enable) {
    if (!enable || !active_) return;
    if (kind_ == Kind::Line) {
        if (x_ == end_x_ && y_ == end_y_) {
            active_ = false;
            return;
        }
        // Both decisions use the same old twice-error. Evaluating the second
        // decision from the already-updated error would change diagonal ties.
        const auto twice_error = static_cast<std::int16_t>(error_ + error_);
        if (twice_error >= dy_) {
            error_ = static_cast<std::int16_t>(error_ + dy_);
            x_ += sx_;
        }
        if (twice_error <= dx_) {
            error_ = static_cast<std::int16_t>(error_ + dx_);
            y_ += sy_;
        }
    } else if (x_ + 1 < window_.x1) {
        ++x_;
        if (kind_ == Kind::Sprite) ++source_x_;
    } else if (y_ + 1 < window_.y1) {
        x_ = window_.x0;
        ++y_;
        if (kind_ == Kind::Sprite) {
            source_x_ = source_start_x_;
            ++source_y_;
        }
    } else {
        active_ = false;
    }
}

} // namespace nano_gpu::hw
