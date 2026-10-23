#include "nano_gpu/demo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace nano_gpu {
namespace {

constexpr long double pi = 3.141592653589793238462643383279502884L;
constexpr std::uint16_t transparent_key = 0xf81f;
constexpr Color background{7, 15, 27};
constexpr Color panel{12, 27, 43};
constexpr Color border{31, 55, 75};
constexpr Color white{227, 236, 245};
constexpr Color cyan{70, 215, 230};
constexpr Color muted{119, 146, 169};
constexpr Color green{132, 226, 156};
constexpr Color amber{247, 187, 92};

struct Vec3 {
    long double x, y, z;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, long double k) { return {a.x * k, a.y * k, a.z * k}; }
long double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
Vec3 unit(Vec3 a) { return a * (1.0L / std::sqrt(dot(a, a))); }

struct Camera {
    Vec3 eye, right, up, forward;
    long double offset_x = 0;
    long double offset_y = 0;

    Camera(Vec3 origin, Vec3 target, long double shift_x = 0, long double shift_y = 0)
        : eye(origin), forward(unit(target - origin)), offset_x(shift_x),
          offset_y(shift_y) {
        right = unit(cross({0, 1, 0}, forward));
        up = cross(forward, right);
    }

    ClipVertex vertex(Vec3 position, long double u = 0, long double v = 0,
                      Color color = white) const {
        const Vec3 relative = position - eye;
        const long double w = dot(relative, forward);
        constexpr long double near_plane = 0.35L;
        constexpr long double far_plane = 55.0L;
        const long double fy = 1.0L / std::tan(65.0L * pi / 360.0L);
        const long double fx = fy * screen_height / screen_width;
        return {dot(relative, right) * fx + offset_x * w,
                dot(relative, up) * fy + offset_y * w,
                far_plane * (w - near_plane) / (far_plane - near_plane), w,
                u, v, static_cast<long double>(color.r),
                static_cast<long double>(color.g), static_cast<long double>(color.b)};
    }
};

void triangle(Pass& pass, const std::array<ClipVertex, 3>& vertices,
              const RenderState& state) {
    for (const auto& clipped : clip_project_triangle(vertices)) {
        if (auto prepared = setup_triangle(clipped, state)) {
            pass.primitives.emplace_back(std::move(*prepared));
        }
    }
}

void quad(Pass& pass, const std::array<ClipVertex, 4>& vertices,
          const RenderState& state) {
    triangle(pass, {vertices[0], vertices[1], vertices[2]}, state);
    triangle(pass, {vertices[0], vertices[2], vertices[3]}, state);
}

ClipVertex screen_vertex(long double x, long double y, long double depth,
                         long double w = 1, long double u = 0, long double v = 0,
                         Color color = white) {
    return {(x / 320.0L - 1.0L) * w, (1.0L - y / 240.0L) * w,
            depth * w, w, u, v, static_cast<long double>(color.r),
            static_cast<long double>(color.g), static_cast<long double>(color.b)};
}

void rect(Pass& pass, Bounds bounds, Color color) {
    pass.primitives.emplace_back(Rect{bounds, overlay_state(color), 0});
}

void line(Pass& pass, Point a, Point b, Color color) {
    pass.primitives.emplace_back(Line{a, b, overlay_state(color), 0});
}

void card(Pass& pass, Bounds bounds) {
    rect(pass, bounds, panel);
    line(pass, {bounds.x0, bounds.y0}, {bounds.x1 - 1, bounds.y0}, border);
    line(pass, {bounds.x0, bounds.y1 - 1}, {bounds.x1 - 1, bounds.y1 - 1}, border);
    line(pass, {bounds.x0, bounds.y0}, {bounds.x0, bounds.y1 - 1}, border);
    line(pass, {bounds.x1 - 1, bounds.y0}, {bounds.x1 - 1, bounds.y1 - 1}, border);
}

struct Glyph {
    char character;
    std::array<unsigned char, 7> rows;
};

// Compact 5x7 bitmap alphabet; cells are baked into POT RGB565 atlases.
constexpr Glyph alphabet[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
    {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {14, 4, 4, 4, 4, 4, 14}},     {'J', {7, 2, 2, 2, 18, 18, 12}},
    {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
    {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},  {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},    {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {30, 1, 1, 14, 1, 1, 30}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 16, 30, 1, 1, 30}},
    {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},
    {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 1, 14}},
    {'/', {1, 2, 2, 4, 8, 8, 16}},       {'-', {0, 0, 0, 31, 0, 0, 0}},
    {'+', {0, 4, 4, 31, 4, 4, 0}},       {':', {0, 4, 4, 0, 4, 4, 0}},
    {'=', {0, 0, 31, 0, 31, 0, 0}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},      {'?', {14, 17, 1, 2, 4, 0, 4}},
    {'[', {14, 8, 8, 8, 8, 8, 14}},      {']', {14, 2, 2, 2, 2, 2, 14}},
};

std::shared_ptr<const Image> font_atlas(Color color, int scale) {
    auto image = std::make_shared<Image>(128 * scale, 64 * scale);
    for (int y = 0; y < image->height(); ++y) {
        for (int x = 0; x < image->width(); ++x) image->at(x, y) = transparent_key;
    }
    for (const auto& glyph : alphabet) {
        const int index = glyph.character - 32;
        const int cell_x = (index % 16) * 8 * scale;
        const int cell_y = (index / 16) * 8 * scale;
        for (int y = 0; y < 7; ++y) {
            for (int x = 0; x < 5; ++x) {
                if ((glyph.rows[y] & (1 << (4 - x))) == 0) continue;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        image->at(cell_x + (x + 1) * scale + sx,
                                  cell_y + y * scale + sy) = pack_rgb565(color);
                    }
                }
            }
        }
    }
    return image;
}

std::shared_ptr<const Image> checker_texture(bool floor) {
    auto image = std::make_shared<Image>(64, 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const bool light = ((x / 8) ^ (y / 8)) & 1;
            Color color = floor ? (light ? Color{99, 145, 160} : Color{24, 52, 72})
                                : (light ? Color{231, 232, 208} : Color{27, 158, 171});
            if (!floor && (x % 16 == 0 || y % 16 == 0)) color = {18, 72, 86};
            image->at(x, y) = pack_rgb565(color);
        }
    }
    return image;
}

std::shared_ptr<const Image> chip_sprite() {
    auto image = std::make_shared<Image>(64, 64);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) image->at(x, y) = transparent_key;
    }
    auto box = [&](int x0, int y0, int x1, int y1, Color color) {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) image->at(x, y) = pack_rgb565(color);
        }
    };
    for (int i = 0; i < 5; ++i) {
        const int p = 19 + i * 6;
        box(7, p, 17, p + 3, muted);
        box(47, p, 57, p + 3, muted);
        box(p, 5, p + 3, 15, muted);
        box(p, 49, p + 3, 59, muted);
    }
    box(15, 13, 49, 51, cyan);
    box(18, 16, 46, 48, Color{14, 68, 83});
    box(20, 18, 44, 21, Color{39, 112, 128});
    box(23, 27, 28, 33, white);
    box(36, 27, 41, 33, white);
    box(26, 39, 38, 42, amber);
    box(18, 44, 22, 48, amber);
    return image;
}

struct Assets {
    std::shared_ptr<const Image> floor = checker_texture(true);
    std::shared_ptr<const Image> cube = checker_texture(false);
    std::shared_ptr<const Image> chip = chip_sprite();
    std::shared_ptr<const Image> font_white = font_atlas(white, 1);
    std::shared_ptr<const Image> font_cyan = font_atlas(cyan, 1);
    std::shared_ptr<const Image> font_muted = font_atlas(muted, 1);
    std::shared_ptr<const Image> font_green = font_atlas(green, 1);
    std::shared_ptr<const Image> font_amber = font_atlas(amber, 1);
    std::shared_ptr<const Image> font_large = font_atlas(white, 2);
};

const Assets& assets() {
    static const Assets result;
    return result;
}

void sprite(Pass& pass, int x, int y, int width, int height,
            std::shared_ptr<const Image> image, int source_x = 0, int source_y = 0) {
    RenderState state = overlay_state();
    state.shade = ShadeMode::TextureReplace;
    state.texture = std::move(image);
    state.color_key = transparent_key;
    pass.primitives.emplace_back(Sprite{{x, y, x + width, y + height}, source_x,
                                        source_y, state, 0});
}

void text(Pass& pass, int x, int y, const std::string& label,
          const std::shared_ptr<const Image>& font, int scale = 1) {
    const int cell = 8 * scale;
    for (unsigned char character : label) {
        if (character != ' ') {
            const int index = (character >= 32 && character < 127 ? character : '?') - 32;
            sprite(pass, x, y, cell, cell, font, (index % 16) * cell,
                   (index / 16) * cell);
        }
        x += cell;
    }
}

void chrome(Pass& pass, const std::string& subtitle, const std::string& footer,
            int scene_number, long double time_seconds) {
    const auto& art = assets();
    rect(pass, {0, 0, 640, 68}, Color{10, 23, 39});
    rect(pass, {20, 21, 25, 42}, cyan);
    line(pass, {20, 67}, {619, 67}, border);
    text(pass, 36, 18, "NANO 20K", art.font_large, 2);
    text(pass, 186, 25, "/ GPU REFERENCE", art.font_cyan);
    text(pass, 36, 45, "RP2350 + TANG NANO 20K", art.font_muted);
    text(pass, 452, 23, "640X480 / RGB565", art.font_white);
    text(pass, 452, 43, "M0 SOFTWARE MODEL", art.font_green);
    line(pass, {20, 435}, {619, 435}, border);
    text(pass, 20, 448, subtitle, art.font_white);
    text(pass, 20, 465, footer, art.font_muted);
    std::ostringstream clock;
    clock << "TIME " << std::fixed << std::setprecision(2)
          << static_cast<double>(std::fmod(std::abs(time_seconds), 1000.0L));
    text(pass, 500, 448, clock.str(), art.font_cyan);
    text(pass, 524, 465, "SCENE " + std::to_string(scene_number) + " / 3", art.font_muted);
}

RenderState textured_state(const std::shared_ptr<const Image>& texture, Bounds scissor) {
    RenderState state;
    state.shade = ShadeMode::TextureReplace;
    state.texture = texture;
    state.address_u = AddressMode::Repeat;
    state.address_v = AddressMode::Repeat;
    state.scissor = scissor;
    return state;
}

void floor_plane(Pass& pass, const Camera& camera, Bounds scissor, long double half_width,
                 long double near_z, long double far_z, long double y) {
    auto state = textured_state(assets().floor, scissor);
    // Even the largest UV is within the M0 [-1024,1024] texel domain.
    quad(pass, {camera.vertex({-half_width, y, near_z}, 0, 0),
                camera.vertex({half_width, y, near_z}, 512, 0),
                camera.vertex({half_width, y, far_z}, 512, 1024),
                camera.vertex({-half_width, y, far_z}, 0, 1024)}, state);
}

Vec3 rotate(Vec3 point, long double yaw, long double pitch) {
    const Vec3 y_rotated{point.x * std::cos(yaw) + point.z * std::sin(yaw), point.y,
                         -point.x * std::sin(yaw) + point.z * std::cos(yaw)};
    return {y_rotated.x, y_rotated.y * std::cos(pitch) - y_rotated.z * std::sin(pitch),
            y_rotated.y * std::sin(pitch) + y_rotated.z * std::cos(pitch)};
}

void cube(Pass& pass, const Camera& camera, Bounds scissor, long double time,
          bool opaque = false) {
    constexpr long double h = 1.35L;
    const std::array<Vec3, 8> local{{{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                                     {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}}};
    const std::array<std::array<int, 4>, 6> faces{{{{0, 1, 2, 3}}, {{5, 4, 7, 6}},
                                                 {{4, 0, 3, 7}}, {{1, 5, 6, 2}},
                                                 {{3, 2, 6, 7}}, {{4, 5, 1, 0}}}};
    const std::array<Vec3, 6> normals{{{0, 0, -1}, {0, 0, 1}, {-1, 0, 0},
                                       {1, 0, 0}, {0, 1, 0}, {0, -1, 0}}};
    const long double yaw = 0.45L + time * 0.55L;
    const long double pitch = -0.17L + 0.08L * std::sin(time * 0.35L);
    const Vec3 light = unit({-0.45L, 0.8L, -0.6L});
    const Vec3 center{0, 0.55L + 0.12L * std::sin(time), 3.0L};
    std::array<Vec3, 8> transformed{};
    for (std::size_t i = 0; i < local.size(); ++i) transformed[i] = rotate(local[i], yaw, pitch) + center;
    for (std::size_t face = 0; face < faces.size(); ++face) {
        auto state = opaque ? RenderState{} : textured_state(assets().cube, scissor);
        state.scissor = scissor;
        state.shade = opaque ? ShadeMode::Flat : ShadeMode::TextureFlatModulate;
        const long double intensity = 0.33L + 0.67L * std::max(0.0L, dot(rotate(normals[face], yaw, pitch), light));
        const auto channel = static_cast<std::uint8_t>(std::round(255.0L * intensity));
        state.color = {channel, channel, channel};
        if (opaque) {
            const std::array<Color, 6> colors{{cyan, amber, Color{128, 104, 234},
                                               green, Color{240, 116, 94}, muted}};
            state.color = modulate(colors[face], state.color);
        }
        const auto& indices = faces[face];
        // All faces are submitted, including hidden faces, exercising actual D24
        // occlusion rather than using CPU painter sorting or back-face removal.
        quad(pass, {camera.vertex(transformed[indices[0]], 0, 64),
                    camera.vertex(transformed[indices[1]], 64, 64),
                    camera.vertex(transformed[indices[2]], 64, 0),
                    camera.vertex(transformed[indices[3]], 0, 0)}, state);
    }
}

void showcase(Pass& pass, long double time) {
    const auto& art = assets();
    const Bounds viewport{198, 80, 620, 420};
    rect(pass, viewport, Color{9, 22, 36});
    const Camera camera({3.4L, 2.8L, -4.5L}, {0, 0.3L, 3}, 0.30L, 0.04L);
    floor_plane(pass, camera, viewport, 8, -3.8L, 24, -1.25L);
    cube(pass, camera, viewport, time);

    card(pass, {20, 90, 184, 262});
    text(pass, 32, 104, "GOURAUD RGB", art.font_cyan);
    RenderState gouraud;
    gouraud.shade = ShadeMode::Gouraud;
    gouraud.scissor = {32, 126, 172, 232};
    triangle(pass, {screen_vertex(103, 130, 0.3L, 1, 0, 0, Color{255, 103, 91}),
                    screen_vertex(39, 229, 0.3L, 1, 0, 0, Color{58, 213, 237}),
                    screen_vertex(168, 229, 0.3L, 1, 0, 0, Color{250, 213, 80})}, gouraud);
    text(pass, 40, 244, "VERTEX COLOR", art.font_muted);

    card(pass, {20, 278, 184, 420});
    text(pass, 32, 292, "FRAME STATE", art.font_cyan);
    text(pass, 32, 316, "COLOR  RGB565", art.font_white);
    text(pass, 32, 336, "DEPTH  24 BIT", art.font_white);
    text(pass, 32, 356, "TEX    NEAREST", art.font_white);
    text(pass, 32, 394, "Z TEST: LESS", art.font_green);
    text(pass, 212, 94, "01 / TEXTURED SCENE", art.font_muted);

    card(pass, {436, 324, 620, 420});
    text(pass, 448, 336, "2D SPRITE", art.font_cyan);
    text(pass, 448, 360, "RGB565", art.font_white);
    text(pass, 448, 378, "KEYED", art.font_white);
    text(pass, 448, 400, "1:1", art.font_muted);
    for (int y = 350; y < 414; y += 8) {
        for (int x = 548; x < 612; x += 8) {
            rect(pass, {x, y, x + 8, y + 8}, ((x / 8 + y / 8) & 1) ? Color{23, 45, 62} : Color{16, 33, 48});
        }
    }
    sprite(pass, 548, 350, 64, 64, art.chip);
    chrome(pass, "PERSPECTIVE TEXTURES + DEPTH", "CPU CLIP + SETUP / GPU RASTER + Z + SHADE", 1, time);
}

void perspective(Pass& pass, long double time) {
    const auto& art = assets();
    const Bounds viewport{20, 82, 620, 420};
    rect(pass, viewport, Color{11, 26, 42});
    const long double sway = 0.5L * std::sin(time * 0.35L);
    const Camera camera({3.1L + sway, 2.3L, -4.5L}, {0, -0.1L, 7});
    floor_plane(pass, camera, viewport, 3.3L, -2.4L, 35, -1.1L);

    // A textured tilted sheet has a large W gradient across both triangles.
    auto state = textured_state(art.cube, viewport);
    quad(pass, {camera.vertex({-2.8L, -0.5L, 1.0L}, 0, 128),
                camera.vertex({-0.7L, -0.5L, 12.0L}, 256, 128),
                camera.vertex({-0.7L, 1.45L, 12.0L}, 256, 0),
                camera.vertex({-2.8L, 1.45L, 1.0L}, 0, 0)}, state);

    RenderState solid;
    solid.scissor = viewport;
    solid.color = amber;
    quad(pass, {camera.vertex({0.65L, -1.1L, 3.5L}),
                camera.vertex({0.95L, -1.1L, 3.5L}),
                camera.vertex({0.95L, 0.75L, 3.5L}),
                camera.vertex({0.65L, 0.75L, 3.5L})}, solid);
    rect(pass, {34, 96, 330, 137}, panel);
    text(pass, 46, 106, "02 / PERSPECTIVE CORRECTION", art.font_cyan);
    text(pass, 46, 121, "INTERPOLATE U/W, V/W AND 1/W", art.font_muted);
    card(pass, {450, 168, 606, 306});
    text(pass, 462, 182, "UV RECOVERY", art.font_cyan);
    text(pass, 462, 209, "Q = 1/W", art.font_white);
    text(pass, 462, 229, "U = UQ/Q", art.font_white);
    text(pass, 462, 249, "V = VQ/Q", art.font_white);
    text(pass, 462, 282, "PER FRAGMENT", art.font_muted);
    rect(pass, {34, 382, 322, 408}, panel);
    text(pass, 46, 391, "NEAREST / REPEAT / SIX-PLANE CLIP", art.font_white);
    chrome(pass, "FORESHORTENED CHECKERBOARD", "RGB565 TEXTURES / PERSPECTIVE-CORRECT UV / D24", 2, time);
}

void overdraw(Pass& pass, long double time) {
    const auto& art = assets();
    const Bounds viewport{20, 82, 620, 420};
    rect(pass, viewport, Color{10, 24, 39});
    constexpr int layers = 28;
    for (int i = 0; i < layers; ++i) {
        const long double radius = 75 + 6.1L * i;
        const long double angle = -pi / 2 + 0.018L * i + 0.08L * std::sin(time * 0.4L);
        const long double depth = 0.15L + i * 0.025L;
        const long double phase = static_cast<long double>(i) / (layers - 1);
        RenderState state = textured_state(art.cube, viewport);
        state.shade = ShadeMode::TextureGouraudModulate;
        const Color a{static_cast<std::uint8_t>(85 + 160 * phase), 220,
                      static_cast<std::uint8_t>(245 - 130 * phase)};
        const Color b{static_cast<std::uint8_t>(220 - 100 * phase),
                      static_cast<std::uint8_t>(120 + 100 * phase), 240};
        const Color c{245, static_cast<std::uint8_t>(180 - 50 * phase), 90};
        std::array<ClipVertex, 3> vertices;
        const std::array<Color, 3> colors{{a, b, c}};
        for (int corner = 0; corner < 3; ++corner) {
            const long double theta = angle + corner * 2 * pi / 3;
            vertices[corner] = screen_vertex(320 + radius * std::cos(theta),
                                             260 + radius * std::sin(theta), depth,
                                             1 + corner * 0.35L,
                                             corner == 1 ? 128 : 0,
                                             corner == 2 ? 128 : 0, colors[corner]);
        }
        // Near geometry is deliberately submitted first: subsequent larger,
        // farther layers must lose depth tests in their overlapping interiors.
        triangle(pass, vertices, state);
    }
    rect(pass, {34, 96, 362, 137}, panel);
    text(pass, 46, 106, "03 / DEPTH-TESTED OVERDRAW", art.font_cyan);
    text(pass, 46, 121, "28 LAYERS / NEAR FIRST / LESS", art.font_muted);
    rect(pass, {34, 381, 346, 409}, panel);
    text(pass, 46, 391, "HIDDEN FRAGMENTS KEEP FRONT COLOR", art.font_white);
    chrome(pass, "OPAQUE LAYERS / REAL D24 OCCLUSION", "GOURAUD TEXTURE MODULATION / NO BLENDING", 3, time);
}

// The hardware slice accepts only prepared flat/Gouraud triangles. Every shape
// here uses those records, including the floor strips and color swatches.
void opaque_scene(Pass& pass, long double time) {
    const Bounds viewport{0, 0, screen_width, screen_height};
    const Camera camera({3.4L, 2.8L, -4.5L}, {0, 0.3L, 3}, 0.28L, 0.04L);
    RenderState floor;
    floor.color = {16, 32, 48};
    quad(pass, {camera.vertex({-9, -1.25L, -3}), camera.vertex({9, -1.25L, -3}),
                camera.vertex({9, -1.25L, 24}), camera.vertex({-9, -1.25L, 24})}, floor);
    for (int strip = -6; strip <= 6; ++strip) {
        floor.color = (strip & 1) ? Color{30, 53, 70} : Color{22, 42, 57};
        const auto x = static_cast<long double>(strip);
        quad(pass, {camera.vertex({x, -1.249L, -3}), camera.vertex({x + 0.035L, -1.249L, -3}),
                    camera.vertex({x + 0.035L, -1.249L, 24}), camera.vertex({x, -1.249L, 24})}, floor);
    }
    cube(pass, camera, viewport, time, true);

    RenderState gradient;
    gradient.shade = ShadeMode::Gouraud;
    triangle(pass, {screen_vertex(106, 98, 0.08L, 1, 0, 0, Color{255, 104, 92}),
                    screen_vertex(24, 342, 0.08L, 1, 0, 0, Color{64, 222, 236}),
                    screen_vertex(190, 342, 0.08L, 1, 0, 0, Color{250, 215, 92})}, gradient);
    constexpr std::array<Color, 8> swatches{{{239, 89, 98}, {242, 161, 83},
        {240, 213, 100}, {132, 226, 156}, {70, 215, 230}, {97, 150, 237},
        {154, 113, 229}, {227, 236, 245}}};
    auto overlay = overlay_state();
    for (std::size_t i = 0; i < swatches.size(); ++i) {
        overlay.color = swatches[i];
        const auto x = 24.0L + 74.0L * i;
        quad(pass, {screen_vertex(x, 436, 0), screen_vertex(x + 62, 436, 0),
                    screen_vertex(x + 62, 456, 0), screen_vertex(x, 456, 0)}, overlay);
    }
}

} // namespace

Pass make_demo_pass(DemoScene scene, int target, long double time_seconds) {
    if (target < 0 || target > 1) throw std::invalid_argument("demo target must be 0 or 1");
    if (!std::isfinite(time_seconds)) throw std::invalid_argument("demo time must be finite");
    Pass pass;
    pass.target = target;
    pass.clear_color = pack_rgb565(background);
    const long double time = std::fmod(time_seconds, 3600.0L);
    switch (scene) {
    case DemoScene::Showcase: showcase(pass, time); break;
    case DemoScene::Perspective: perspective(pass, time); break;
    case DemoScene::Overdraw: overdraw(pass, time); break;
    case DemoScene::Opaque: opaque_scene(pass, time); break;
    default: throw std::invalid_argument("unknown demo scene");
    }
    return pass;
}

const char* scene_name(DemoScene scene) {
    switch (scene) {
    case DemoScene::Showcase: return "showcase";
    case DemoScene::Perspective: return "perspective";
    case DemoScene::Overdraw: return "overdraw";
    case DemoScene::Opaque: return "opaque";
    }
    return "unknown";
}

} // namespace nano_gpu
