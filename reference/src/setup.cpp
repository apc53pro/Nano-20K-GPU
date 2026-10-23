#include "nano_gpu/reference.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace nano_gpu {
namespace {

using Wide = __int128;

std::int64_t narrow(Wide value) {
    if (value < std::numeric_limits<std::int64_t>::min() ||
        value > std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error("signed 64-bit arithmetic overflow");
    }
    return static_cast<std::int64_t>(value);
}

Wide magnitude(Wide value) { return value < 0 ? -value : value; }

Wide rounded_quotient(Wide numerator, Wide denominator) {
    if (denominator == 0) {
        throw std::invalid_argument("division by zero");
    }
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    Wide quotient = numerator / denominator;
    const Wide twice_remainder = 2 * magnitude(numerator % denominator);
    if (twice_remainder > denominator ||
        (twice_remainder == denominator && quotient % 2 != 0)) {
        quotient += numerator < 0 ? -1 : 1;
    }
    return quotient;
}

bool textured(ShadeMode mode) {
    switch (mode) {
    case ShadeMode::Flat:
    case ShadeMode::Gouraud:
        return false;
    case ShadeMode::TextureReplace:
    case ShadeMode::TextureFlatModulate:
    case ShadeMode::TextureGouraudModulate:
        return true;
    }
    throw std::invalid_argument("unsupported shading mode");
}

bool needs_texture(const RenderState& state) {
    return textured(state.shade) && (state.color_write || state.color_key.has_value());
}

bool needs_color_planes(const RenderState& state) {
    return state.color_write && (state.shade == ShadeMode::Gouraud ||
                                 state.shade == ShadeMode::TextureGouraudModulate);
}

void validate_state(const RenderState& state) {
    const bool is_textured = textured(state.shade);
    switch (state.depth_func) {
    case DepthFunc::Less:
    case DepthFunc::LessEqual:
    case DepthFunc::Always:
        break;
    default:
        throw std::invalid_argument("unsupported depth function");
    }
    for (const auto mode : {state.address_u, state.address_v}) {
        if (mode != AddressMode::Clamp && mode != AddressMode::Repeat) {
            throw std::invalid_argument("unsupported texture address mode");
        }
    }
    if (state.color_key && !is_textured) {
        throw std::invalid_argument("color key requires textured shading");
    }
    if (needs_texture(state) && (!state.texture || !state.texture->texture_compatible())) {
        throw std::invalid_argument("texture is missing or incompatible with the profile");
    }
}

void require_finite(long double value, const char* message) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(message);
    }
}

void require_range(long double value, long double low, long double high,
                   const char* message) {
    if (!std::isfinite(value) || value < low || value > high) {
        throw std::invalid_argument(message);
    }
}

std::int64_t area(const std::array<Point, 3>& points) {
    return static_cast<std::int64_t>(points[1].x - points[0].x) *
               (points[2].y - points[0].y) -
           static_cast<std::int64_t>(points[1].y - points[0].y) *
               (points[2].x - points[0].x);
}

Edge make_edge(Point from, Point to) {
    const std::int64_t dx = static_cast<std::int64_t>(to.x) - from.x;
    const std::int64_t dy = static_cast<std::int64_t>(to.y) - from.y;
    const std::int64_t c = dy * from.x - dx * from.y;
    return {static_cast<std::int32_t>(-dy), static_cast<std::int32_t>(dx),
            static_cast<std::int32_t>(c), dy < 0 || (dy == 0 && dx > 0)};
}

std::int64_t edge_value(const Edge& edge, Point sample) {
    return static_cast<std::int64_t>(edge.a) * sample.x +
           static_cast<std::int64_t>(edge.b) * sample.y + edge.c;
}

int floor_divide(int numerator, int denominator) {
    const int quotient = numerator / denominator;
    return quotient - (numerator % denominator < 0 ? 1 : 0);
}

int ceil_divide(int numerator, int denominator) {
    return -floor_divide(-numerator, denominator);
}

Bounds triangle_bounds(const std::array<Point, 3>& points, Bounds scissor) {
    const int min_x = std::min({points[0].x, points[1].x, points[2].x});
    const int min_y = std::min({points[0].y, points[1].y, points[2].y});
    const int max_x = std::max({points[0].x, points[1].x, points[2].x});
    const int max_y = std::max({points[0].y, points[1].y, points[2].y});
    const Bounds bounds{std::max(0, ceil_divide(min_x - 8, 16)),
                        std::max(0, ceil_divide(min_y - 8, 16)),
                        std::min(screen_width, floor_divide(max_x - 8, 16) + 1),
                        std::min(screen_height, floor_divide(max_y - 8, 16) + 1)};
    return intersect(bounds, scissor);
}

bool same_bounds(Bounds a, Bounds b) {
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

void preflight(const Plane& plane, Bounds bounds) {
    if (bounds.empty()) {
        return;
    }
    const Wide bound = magnitude(static_cast<Wide>(plane.seed)) +
                       static_cast<Wide>(bounds.x1 - bounds.x0 - 1) *
                           magnitude(static_cast<Wide>(plane.step_x)) +
                       static_cast<Wide>(bounds.y1 - bounds.y0 - 1) *
                           magnitude(static_cast<Wide>(plane.step_y));
    if (bound > std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error("attribute plane exceeds the conservative s64 budget");
    }
}

Plane make_plane(const Triangle& triangle, const std::array<std::int64_t, 3>& values) {
    const auto& p = triangle.snapped;
    const Wide denominator = area(p);
    const Wide difference_1 = static_cast<Wide>(values[1]) - values[0];
    const Wide difference_2 = static_cast<Wide>(values[2]) - values[0];
    const Wide numerator_x = 16 * (difference_1 * (p[2].y - p[0].y) -
                                    difference_2 * (p[1].y - p[0].y));
    const Wide numerator_y = 16 * (static_cast<Wide>(p[1].x - p[0].x) * difference_2 -
                                    static_cast<Wide>(p[2].x - p[0].x) * difference_1);
    const Point anchor{16 * triangle.bounds.x0 + 8, 16 * triangle.bounds.y0 + 8};
    const Wide numerator_seed =
        static_cast<Wide>(values[0]) * edge_value(triangle.edges[1], anchor) +
        static_cast<Wide>(values[1]) * edge_value(triangle.edges[2], anchor) +
        static_cast<Wide>(values[2]) * edge_value(triangle.edges[0], anchor);
    Plane result{narrow(rounded_quotient(numerator_seed, denominator)),
                 narrow(rounded_quotient(numerator_x, denominator)),
                 narrow(rounded_quotient(numerator_y, denominator))};
    preflight(result, triangle.bounds);
    return result;
}

template <class Function>
Plane vertex_plane(const Triangle& triangle, const std::array<ScreenVertex, 3>& vertices,
                   Function value) {
    std::array<std::int64_t, 3> integers{};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        integers[i] = round_even(value(vertices[i], i));
    }
    return make_plane(triangle, integers);
}

auto clip_key(const ClipVertex& vertex) {
    return std::tie(vertex.x, vertex.y, vertex.z, vertex.w, vertex.u, vertex.v,
                    vertex.r, vertex.g, vertex.b);
}

bool same_vertex(const ClipVertex& a, const ClipVertex& b) {
    return clip_key(a) == clip_key(b);
}

long double clip_distance(const ClipVertex& vertex, int plane) {
    long double distance = 0;
    switch (plane) {
    case 0: distance = vertex.x + vertex.w; break;
    case 1: distance = vertex.w - vertex.x; break;
    case 2: distance = vertex.y + vertex.w; break;
    case 3: distance = vertex.w - vertex.y; break;
    case 4: distance = vertex.z; break;
    case 5: distance = vertex.w - vertex.z; break;
    default: throw std::invalid_argument("unsupported clip plane");
    }
    if (!std::isfinite(distance)) {
        throw std::overflow_error("clip-plane distance overflow");
    }
    return distance;
}

ClipVertex intersection(ClipVertex a, ClipVertex b, int plane) {
    // Canonical endpoint evaluation makes a reversed shared edge produce the
    // same floating-point intersection rather than merely a nearby point.
    if (clip_key(b) < clip_key(a)) {
        std::swap(a, b);
    }
    const long double distance_a = clip_distance(a, plane);
    const long double distance_b = clip_distance(b, plane);
    const long double a_magnitude = std::fabs(distance_a);
    const long double b_magnitude = std::fabs(distance_b);
    long double t;
    if (a_magnitude == 0) {
        t = 0;
    } else if (b_magnitude == 0) {
        t = 1;
    } else if (a_magnitude >= b_magnitude) {
        t = 1 / (1 + b_magnitude / a_magnitude);
    } else {
        const long double ratio = a_magnitude / b_magnitude;
        t = ratio / (1 + ratio);
    }
    const auto lerp = [t](long double first, long double second) {
        const long double result = (1 - t) * first + t * second;
        if (!std::isfinite(result)) {
            throw std::overflow_error("clip interpolation overflow");
        }
        return result;
    };
    ClipVertex result{lerp(a.x, b.x), lerp(a.y, b.y), lerp(a.z, b.z), lerp(a.w, b.w),
                      lerp(a.u, b.u), lerp(a.v, b.v), lerp(a.r, b.r),
                      lerp(a.g, b.g), lerp(a.b, b.b)};
    // Put the constrained component exactly on the current plane.
    switch (plane) {
    case 0: result.x = -result.w; break;
    case 1: result.x = result.w; break;
    case 2: result.y = -result.w; break;
    case 3: result.y = result.w; break;
    case 4: result.z = 0; break;
    case 5: result.z = result.w; break;
    }
    return result;
}

ScreenVertex project(const ClipVertex& vertex) {
    if (!(vertex.w > 0)) {
        throw std::invalid_argument("remaining clip vertex has nonpositive W");
    }
    const long double x = 320 * (vertex.x / vertex.w + 1);
    const long double y = 240 * (1 - vertex.y / vertex.w);
    const long double depth = vertex.z / vertex.w;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(depth)) {
        throw std::overflow_error("projection overflow");
    }
    // Clipping has established the exact domain; remove roundoff at its edges.
    return {std::clamp(x, 0.0L, 640.0L), std::clamp(y, 0.0L, 480.0L),
            std::clamp(depth, 0.0L, 1.0L), vertex.w, vertex.u, vertex.v,
            vertex.r, vertex.g, vertex.b};
}

} // namespace

Bounds intersect(Bounds a, Bounds b) {
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0),
            std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

std::int64_t round_even(long double value) {
    require_finite(value, "cannot round a nonfinite value");
    const long double low = -std::ldexp(1.0L, 63);
    const long double high = std::ldexp(1.0L, 63);
    long double rounded = std::floor(value);
    const long double fraction = value - rounded;
    if (fraction > 0.5L || (fraction == 0.5L && std::fmod(rounded, 2.0L) != 0)) {
        rounded += 1;
    }
    if (rounded < low || rounded >= high) {
        throw std::overflow_error("rounded value does not fit signed 64 bits");
    }
    return static_cast<std::int64_t>(rounded);
}

std::int64_t divide_round_even(std::int64_t numerator, std::int64_t denominator) {
    return narrow(rounded_quotient(numerator, denominator));
}

std::int64_t Plane::at(int delta_x, int delta_y) const {
    return narrow(static_cast<Wide>(seed) + static_cast<Wide>(delta_x) * step_x +
                  static_cast<Wide>(delta_y) * step_y);
}

std::uint32_t depth_code(std::int64_t q16) {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        divide_round_even(q16, 65536), 0, depth_max));
}

std::uint8_t color_code(std::int64_t q16) {
    return static_cast<std::uint8_t>(std::clamp<std::int64_t>(
        divide_round_even(q16, 65536), 0, 255));
}

int floor_q16(std::int32_t value) { return floor_divide(value, 65536); }

std::optional<Triangle> setup_triangle(std::array<ScreenVertex, 3> vertices,
                                      const RenderState& state) {
    validate_state(state);
    const bool use_depth = state.depth_test || state.depth_write;
    const bool use_colors = needs_color_planes(state);
    const bool use_texture = needs_texture(state);
    Triangle triangle;
    triangle.state = state;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto& vertex = vertices[i];
        require_range(vertex.x, 0, screen_width, "screen X outside viewport");
        require_range(vertex.y, 0, screen_height, "screen Y outside viewport");
        require_finite(vertex.w, "nonfinite W");
        if (!(vertex.w > 0)) {
            throw std::invalid_argument("W must be positive");
        }
        if (use_depth) {
            require_range(vertex.depth, 0, 1, "depth outside normalized range");
        }
        if (use_colors) {
            require_range(vertex.r, 0, 255, "red vertex color outside channel range");
            require_range(vertex.g, 0, 255, "green vertex color outside channel range");
            require_range(vertex.b, 0, 255, "blue vertex color outside channel range");
        }
        if (use_texture) {
            require_range(vertex.u, -1024, 1024, "U outside supported texel range");
            require_range(vertex.v, -1024, 1024, "V outside supported texel range");
        }
        triangle.snapped[i] = {static_cast<int>(round_even(16 * vertex.x)),
                               static_cast<int>(round_even(16 * vertex.y))};
    }
    const auto signed_area = area(triangle.snapped);
    if (signed_area == 0) {
        return std::nullopt;
    }
    if (signed_area < 0) {
        std::swap(triangle.snapped[1], triangle.snapped[2]);
        std::swap(vertices[1], vertices[2]);
    }
    for (std::size_t i = 0; i < triangle.edges.size(); ++i) {
        triangle.edges[i] = make_edge(triangle.snapped[i], triangle.snapped[(i + 1) % 3]);
    }
    triangle.bounds = triangle_bounds(triangle.snapped, state.scissor);
    if (triangle.bounds.empty()) {
        return std::nullopt;
    }
    if (use_depth) {
        triangle.z = vertex_plane(triangle, vertices, [](const ScreenVertex& vertex, std::size_t) {
            return vertex.depth * depth_max * 65536.0L;
        });
    }
    if (use_colors) {
        triangle.r = vertex_plane(triangle, vertices, [](const ScreenVertex& vertex, std::size_t) {
            return vertex.r * 65536.0L;
        });
        triangle.g = vertex_plane(triangle, vertices, [](const ScreenVertex& vertex, std::size_t) {
            return vertex.g * 65536.0L;
        });
        triangle.b = vertex_plane(triangle, vertices, [](const ScreenVertex& vertex, std::size_t) {
            return vertex.b * 65536.0L;
        });
    }
    if (use_texture) {
        const auto w_minmax = std::minmax({vertices[0].w, vertices[1].w, vertices[2].w});
        if (w_minmax.second / w_minmax.first > 64) {
            throw std::invalid_argument("triangle W ratio exceeds 64");
        }
        std::array<long double, 3> q{};
        for (std::size_t i = 0; i < q.size(); ++i) {
            q[i] = 0.5L * (w_minmax.first / vertices[i].w);
        }
        triangle.q = vertex_plane(triangle, vertices, [&q](const ScreenVertex&, std::size_t i) {
            return std::ldexp(q[i], 37);
        });
        triangle.u = vertex_plane(triangle, vertices, [&q](const ScreenVertex& vertex, std::size_t i) {
            return std::ldexp(vertex.u * q[i], 27);
        });
        triangle.v = vertex_plane(triangle, vertices, [&q](const ScreenVertex& vertex, std::size_t i) {
            return std::ldexp(vertex.v * q[i], 27);
        });
    }
    validate_triangle(triangle);
    return triangle;
}

std::vector<std::array<ScreenVertex, 3>> clip_project_triangle(
    const std::array<ClipVertex, 3>& vertices) {
    for (const auto& vertex : vertices) {
        for (const auto value : {vertex.x, vertex.y, vertex.z, vertex.w, vertex.u,
                                 vertex.v, vertex.r, vertex.g, vertex.b}) {
            require_finite(value, "nonfinite clip vertex");
        }
    }
    std::vector<ClipVertex> polygon(vertices.begin(), vertices.end());
    for (int plane = 0; plane < 6 && !polygon.empty(); ++plane) {
        std::vector<ClipVertex> output;
        const auto append = [&output](const ClipVertex& vertex) {
            if (output.empty() || !same_vertex(output.back(), vertex)) {
                output.push_back(vertex);
            }
        };
        ClipVertex previous = polygon.back();
        bool previous_inside = clip_distance(previous, plane) >= 0;
        for (const auto& current : polygon) {
            const bool current_inside = clip_distance(current, plane) >= 0;
            if (previous_inside != current_inside) {
                append(intersection(previous, current, plane));
            }
            if (current_inside) {
                append(current);
            }
            previous = current;
            previous_inside = current_inside;
        }
        if (output.size() > 1 && same_vertex(output.front(), output.back())) {
            output.pop_back();
        }
        polygon = std::move(output);
    }
    if (polygon.size() < 3) {
        return {};
    }
    std::vector<ScreenVertex> projected;
    projected.reserve(polygon.size());
    for (const auto& vertex : polygon) {
        projected.push_back(project(vertex));
    }
    std::vector<std::array<ScreenVertex, 3>> result;
    result.reserve(projected.size() - 2);
    for (std::size_t i = 1; i + 1 < projected.size(); ++i) {
        result.push_back({projected.front(), projected[i], projected[i + 1]});
    }
    return result;
}

void validate_triangle(const Triangle& triangle) {
    validate_state(triangle.state);
    for (const auto point : triangle.snapped) {
        if (point.x < 0 || point.x > 16 * screen_width ||
            point.y < 0 || point.y > 16 * screen_height) {
            throw std::invalid_argument("snapped triangle vertex outside Q4 viewport");
        }
    }
    if (area(triangle.snapped) <= 0) {
        throw std::invalid_argument("triangle area must be positive");
    }
    for (std::size_t i = 0; i < triangle.edges.size(); ++i) {
        const auto expected = make_edge(triangle.snapped[i], triangle.snapped[(i + 1) % 3]);
        const auto actual = triangle.edges[i];
        if (actual.a != expected.a || actual.b != expected.b || actual.c != expected.c ||
            actual.inclusive != expected.inclusive) {
            throw std::invalid_argument("triangle edge does not match snapped geometry");
        }
    }
    if (!same_bounds(triangle.bounds, triangle_bounds(triangle.snapped, triangle.state.scissor))) {
        throw std::invalid_argument("triangle bounds or immutable sample anchor are inconsistent");
    }
    if (triangle.state.depth_test || triangle.state.depth_write) {
        preflight(triangle.z, triangle.bounds);
    }
    if (needs_color_planes(triangle.state)) {
        preflight(triangle.r, triangle.bounds);
        preflight(triangle.g, triangle.bounds);
        preflight(triangle.b, triangle.bounds);
    }
    if (needs_texture(triangle.state)) {
        preflight(triangle.q, triangle.bounds);
        preflight(triangle.u, triangle.bounds);
        preflight(triangle.v, triangle.bounds);
    }
}

bool covered(const Triangle& triangle, int x, int y) {
    if (x < 0 || x >= screen_width || y < 0 || y >= screen_height ||
        !triangle.bounds.contains(x, y) || !triangle.state.scissor.contains(x, y)) {
        return false;
    }
    const Point sample{16 * x + 8, 16 * y + 8};
    for (const auto& edge : triangle.edges) {
        const auto value = edge_value(edge, sample);
        if (value < 0 || (value == 0 && !edge.inclusive)) {
            return false;
        }
    }
    return true;
}

Point texture_coordinates(const Triangle& triangle, int x, int y) {
    if (!needs_texture(triangle.state)) {
        throw std::invalid_argument("triangle does not require UV reconstruction");
    }
    if (x < 0 || x >= screen_width || y < 0 || y >= screen_height ||
        !triangle.bounds.contains(x, y)) {
        throw std::invalid_argument("UV sample outside the primitive's viewport bounds");
    }
    const int dx = x - triangle.bounds.x0;
    const int dy = y - triangle.bounds.y0;
    const auto q = triangle.q.at(dx, dy);
    if (q < (std::int64_t{1} << 29) || q > (std::int64_t{1} << 37)) {
        throw std::invalid_argument("sample reciprocal W outside supported domain");
    }
    const auto reconstruct = [q](std::int64_t value) {
        const Wide result = rounded_quotient(static_cast<Wide>(value) * (Wide{1} << 26), q);
        if (result < std::numeric_limits<std::int32_t>::min() ||
            result > std::numeric_limits<std::int32_t>::max()) {
            throw std::overflow_error("reconstructed UV does not fit signed Q16");
        }
        return static_cast<int>(result);
    };
    return {reconstruct(triangle.u.at(dx, dy)), reconstruct(triangle.v.at(dx, dy))};
}

std::vector<Point> line_pixels(Point a, Point b) {
    for (const auto point : {a, b}) {
        if (point.x < 0 || point.x >= screen_width || point.y < 0 || point.y >= screen_height) {
            throw std::invalid_argument("line endpoint outside viewport");
        }
    }
    if (std::tie(b.x, b.y) < std::tie(a.x, a.y)) {
        std::swap(a, b);
    }
    const int dx = std::abs(b.x - a.x);
    const int sx = a.x < b.x ? 1 : -1;
    const int dy = -std::abs(b.y - a.y);
    const int sy = a.y < b.y ? 1 : -1;
    int error = dx + dy;
    std::vector<Point> result;
    result.reserve(static_cast<std::size_t>(std::max(dx, -dy)) + 1);
    for (;;) {
        result.push_back(a);
        if (a == b) {
            break;
        }
        const int twice_error = 2 * error;
        if (twice_error >= dy) {
            error += dy;
            a.x += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            a.y += sy;
        }
    }
    return result;
}

} // namespace nano_gpu
