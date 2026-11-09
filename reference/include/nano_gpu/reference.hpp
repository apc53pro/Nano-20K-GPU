#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace nano_gpu {

constexpr int screen_width = 640;
constexpr int screen_height = 480;
constexpr int tile_size = 32;
constexpr int tile_columns = 20;
constexpr int tile_rows = 15;
constexpr int tile_count = tile_columns * tile_rows;
constexpr std::uint32_t depth_max = 0x00ffffff;

struct Point {
    int x = 0;
    int y = 0;
};
inline bool operator==(Point a, Point b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(Point a, Point b) { return !(a == b); }

struct Bounds {
    int x0 = 0;
    int y0 = 0;
    int x1 = screen_width;
    int y1 = screen_height;
    bool empty() const { return x0 >= x1 || y0 >= y1; }
    bool contains(int x, int y) const { return x >= x0 && y >= y0 && x < x1 && y < y1; }
};
Bounds intersect(Bounds a, Bounds b);

struct Color {
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
};
Color expand_rgb565(std::uint16_t value);
std::uint16_t pack_rgb565(Color color);
Color modulate(Color texture, Color color);

enum class AddressMode { Clamp, Repeat };
enum class DepthFunc { Less, LessEqual, Always };
enum class ShadeMode { Flat, Gouraud, TextureReplace, TextureFlatModulate, TextureGouraudModulate };
enum class LoadOp { Load, Clear };
// Discard leaves the external depth attachment unchanged and invalid.
enum class StoreOp { Store, Discard };
enum class RenderMode { Tiled, TiledReverse, Monolithic, HardwareBlocks };

// Generic bounded RGB565 image. Texture bindings additionally require POT sizes.
class Image {
public:
    Image(int width, int height, int pitch_bytes = 0);
    // Metadata carried by a fetched GPU descriptor. It has no host pixels;
    // sampling it is an error, so the system model must use its SDRAM port.
    static std::shared_ptr<const Image> descriptor(int width, int height, int pitch_bytes = 0);
    int width() const { return width_; }
    int height() const { return height_; }
    int pitch_bytes() const { return pitch_bytes_; }
    std::uint16_t& at(int x, int y);
    std::uint16_t at(int x, int y) const;
    std::uint16_t sample(int x, int y, AddressMode u, AddressMode v) const;
    bool texture_compatible() const;
private:
    Image(int width, int height, int pitch_bytes, bool allocate_pixels);
    int width_;
    int height_;
    int pitch_bytes_;
    std::vector<std::uint16_t> pixels_;
};

struct RenderState {
    Bounds scissor{};
    bool color_write = true;
    bool depth_test = true;
    bool depth_write = true;
    DepthFunc depth_func = DepthFunc::Less;
    ShadeMode shade = ShadeMode::Flat;
    Color color{};
    std::shared_ptr<const Image> texture;
    AddressMode address_u = AddressMode::Clamp;
    AddressMode address_v = AddressMode::Clamp;
    std::optional<std::uint16_t> color_key;
};
RenderState overlay_state(Color color = {});

struct ScreenVertex {
    long double x = 0;
    long double y = 0;
    long double depth = 0; // Normalized projected depth, not eye-space Z.
    long double w = 1;
    long double u = 0;
    long double v = 0;
    long double r = 255;
    long double g = 255;
    long double b = 255;
};

struct ClipVertex {
    long double x = 0;
    long double y = 0;
    long double z = 0;
    long double w = 1;
    long double u = 0;
    long double v = 0;
    long double r = 255;
    long double g = 255;
    long double b = 255;
};

struct Plane {
    std::int64_t seed = 0;
    std::int64_t step_x = 0;
    std::int64_t step_y = 0;
    std::int64_t at(int delta_x, int delta_y) const;
};
struct Edge {
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    bool inclusive = false;
};
// The submitted vertices of a GPU-setup (kind 4) flat triangle, in record
// order: Q4 positions within 0..16383 x 0..8191 and depths as D24 codes with
// eight fractional bits. `bounds` (pixels, each field 0..2047) is the drawing
// rectangle: the triangle is drawn within bounds and viewport, so it acts as
// a scissor; usually it is the triangle's pixel-center bounds.
// vertex_triangle_setup() defines the triangle the GPU derives from them.
struct VertexSource {
    std::array<Point, 3> points{};
    std::array<std::uint32_t, 3> depth{};
    Bounds bounds{};
    bool cull_negative = false;
};
// Pixel-center bounds of three Q4 vertices: the usual VertexSource bounds.
Bounds vertex_bounds(const std::array<Point, 3>& points);
struct Triangle {
    std::array<Point, 3> snapped{}; // Q4 subpixel coordinates.
    std::array<Edge, 3> edges{};
    Bounds bounds{};              // Also defines the immutable plane anchor.
    RenderState state{};
    Plane z{}, r{}, g{}, b{}, q{}, u{}, v{};
    // Set when the GPU computes this triangle's setup (record kind 4).
    std::optional<VertexSource> source;
};
struct Rect {
    Bounds bounds{};
    RenderState state = overlay_state();
    std::uint32_t depth = 0;
};
struct Line {
    Point a{}, b{};
    RenderState state = overlay_state();
    std::uint32_t depth = 0;
};
struct Sprite {
    Bounds bounds{};
    int source_x = 0;
    int source_y = 0;
    RenderState state = overlay_state();
    std::uint32_t depth = 0;
};
using Primitive = std::variant<Triangle, Rect, Line, Sprite>;
using TileBins = std::array<std::vector<std::size_t>, tile_count>;

std::int64_t round_even(long double value);
std::int64_t divide_round_even(std::int64_t numerator, std::int64_t denominator);
std::uint32_t depth_code(std::int64_t q16);
std::uint8_t color_code(std::int64_t q16);
int floor_q16(std::int32_t value);
std::optional<Triangle> setup_triangle(std::array<ScreenVertex, 3> vertices,
                                      const RenderState& state = {});
// GPU triangle setup for kind-4 records. Returns no triangle for zero area,
// culled negative area or empty bounds. The state must be flat and untextured;
// its scissor is replaced by source.bounds within the viewport. Depth planes
// use Q16 D24 codes:
//   G = sign(N) * min(floor(|N|*4096/area + 1/2), 2^42-1), with
//   N_x = D1*(Y2-Y0) - D2*(Y1-Y0), N_y = D2*(X1-X0) - D1*(X2-X0), Di = Zi-Z0;
//   value(x,y) = Z0*256 + floor((SX*G_x + SY*G_y + 8)/16),
//   SX = 16x+8-X0, SY = 16y+8-Y0, anchored at the bounds corner.
std::optional<Triangle> vertex_triangle_setup(const VertexSource& source,
                                              const RenderState& state);
std::vector<std::array<ScreenVertex, 3>> clip_project_triangle(
    const std::array<ClipVertex, 3>& vertices);
void validate_triangle(const Triangle& triangle);
bool covered(const Triangle& triangle, int x, int y);
Point texture_coordinates(const Triangle& triangle, int x, int y); // Signed Q16.
std::vector<Point> line_pixels(Point a, Point b);
Bounds primitive_bounds(const Primitive& primitive);
TileBins bin_primitives(const std::vector<Primitive>& primitives);

struct Pass {
    int target = 1;
    LoadOp color_load = LoadOp::Clear;
    LoadOp depth_load = LoadOp::Clear;
    StoreOp depth_store = StoreOp::Store;
    // Tiles without primitives keep their attachment contents (no clear,
    // load or store). Requires valid target color, like a color LOAD.
    bool skip_empty_tiles = false;
    std::uint16_t clear_color = 0;
    std::uint32_t clear_depth = depth_max;
    std::vector<Primitive> primitives;
};

struct Statistics {
    std::uint64_t tile_references = 0;
    std::uint64_t tiles_processed = 0;
    std::uint64_t candidates = 0;
    std::uint64_t covered_fragments = 0;
    std::uint64_t depth_rejected = 0;
    std::uint64_t keyed_rejected = 0;
    std::uint64_t texture_samples = 0;
    std::uint64_t color_writes = 0;
    std::uint64_t depth_writes = 0;
    // Clock counts for the selected rendering block model. SDRAM, record
    // fetch, host transport, and display timing are not included.
    std::uint64_t hardware_cycles = 0;
    std::uint64_t hardware_stalls = 0;
    std::uint64_t hardware_setup_cycles = 0;
    std::uint64_t hardware_forwarded_reads = 0;
    std::uint64_t hardware_perspective_cycles = 0;
    std::uint64_t hardware_texture_cycles = 0;
    std::uint64_t hardware_texture_hits = 0;
    std::uint64_t hardware_texture_misses = 0;
    std::uint64_t hardware_texture_refill_words = 0;
    // Most recent modeled copy; kept separate from last-pass raster counters.
    std::uint64_t hardware_copy_cycles = 0;
    std::uint64_t hardware_copy_words = 0;
};

// Synchronous logical reference. The provisional binary transport is not modeled.
class Renderer {
public:
    Renderer();
    void reset();
    void render(const Pass& pass, RenderMode mode = RenderMode::Tiled);
    void copy_rect(int source, int target, Point source_origin, Point destination_origin,
                   int width, int height, RenderMode mode = RenderMode::Tiled);
    void copy_image(const Image& source, int target, Point source_origin,
                    Point destination_origin, int width, int height,
                    RenderMode mode = RenderMode::Tiled);
    void queue_present(int target);
    void vertical_blank();
    void abort();
    int front_id() const { return front_; }
    int back_id() const { return 1 - front_; }
    bool presentation_pending() const { return pending_.has_value(); }
    bool color_valid(int id) const;
    bool depth_valid() const { return depth_valid_; }
    const std::vector<std::uint16_t>& color(int id) const;
    const std::vector<std::uint16_t>& front_color() const { return color(front_); }
    const std::vector<std::uint32_t>& depth() const { return depth_; }
    const Statistics& statistics() const { return statistics_; }
private:
    std::array<std::vector<std::uint16_t>, 2> color_;
    std::vector<std::uint32_t> depth_;
    std::array<bool, 2> color_valid_{{true, true}};
    bool depth_valid_ = true;
    int front_ = 0;
    std::optional<int> pending_;
    Statistics statistics_{};
    void require_writable(int target) const;
};

} // namespace nano_gpu
