#include "nano_gpu/hw/tile_pipeline.hpp"

#include "nano_gpu/hw/bsram.hpp"
#include "nano_gpu/hw/device.hpp"
#include "nano_gpu/hw/mult18x18.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nano_gpu::hw {
namespace {

constexpr std::size_t tile_words = tile_size * tile_size;
constexpr std::size_t frame_words = screen_width * screen_height;

// Host checks are outside the modeled datapath. The s64 adder itself has no
// widening multiplier/divider; legal prepared records cannot overflow it.
std::int64_t add64(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) {
        throw std::overflow_error("opaque tile signed 64-bit adder overflow");
    }
    return a + b;
}

std::int32_t narrow32(std::int64_t value) {
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
        throw std::overflow_error("opaque tile signed 32-bit edge overflow");
    }
    return static_cast<std::int32_t>(value);
}

std::int32_t add32(std::int32_t a, std::int32_t b) {
    return narrow32(static_cast<std::int64_t>(a) + b);
}

// Division by 2^16 is wiring plus tie detection and an incrementer. Saturation
// before narrowing prevents a large positive accumulator from wrapping around.
std::uint32_t quantize_q16(std::int64_t value, std::uint32_t maximum) {
    if (value <= 0) return 0;
    const auto bits = static_cast<std::uint64_t>(value);
    auto integer = bits >> 16;
    const auto fraction = bits & 0xffffu;
    if (fraction > 0x8000u || (fraction == 0x8000u && (integer & 1u))) ++integer;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(integer, maximum));
}

std::uint16_t pack_color(Color color) {
    return static_cast<std::uint16_t>(((color.r >> 3) << 11) |
                                      ((color.g >> 2) << 5) | (color.b >> 3));
}

bool depth_accept(DepthFunc function, std::uint32_t incoming, std::uint32_t stored) {
    switch (function) {
    case DepthFunc::Less: return incoming < stored;
    case DepthFunc::LessEqual: return incoming <= stored;
    case DepthFunc::Always: return true;
    }
    throw std::invalid_argument("unsupported opaque tile depth comparison");
}

void require_depth(std::uint32_t value) {
    if (value > depth_max) throw std::out_of_range("opaque tile depth exceeds D24");
}

void require_origin(Point origin) {
    if (origin.x < 0 || origin.x >= screen_width || origin.x % tile_size != 0 ||
        origin.y < 0 || origin.y >= screen_height || origin.y % tile_size != 0) {
        throw std::invalid_argument("opaque tile origin must name a visible 32x32 tile");
    }
}

void require_opaque(const Triangle& triangle) {
    if ((triangle.state.shade != ShadeMode::Flat &&
         triangle.state.shade != ShadeMode::Gouraud) ||
        triangle.state.texture || triangle.state.color_key) {
        throw std::invalid_argument("hardware opaque slice supports only untextured flat/Gouraud triangles");
    }
    // This is CPU-side prepared-record validation, not GPU triangle setup.
    validate_triangle(triangle);
}

} // namespace

struct OpaqueTile::Impl {
    enum class State { Idle, EdgeLaunch, EdgeWait, EdgeCapture, OffsetY, OffsetX, Raster, Drain };

    struct Pending {
        bool valid = false;
        std::uint16_t address = 0;
        std::uint16_t color = 0;
        std::uint32_t depth = 0;
        bool color_write = false;
        bool depth_write = false;
        bool depth_test = false;
        DepthFunc function = DepthFunc::Less;
        bool forwarded = false;
        std::uint32_t forwarded_depth = 0;
    };

    Sdp1kx16 color_bank;
    Sdp1kx16 low_bank;
    Sdp1kx16 high_bank;
    Mult18x18 multiplier;
    TileStatistics counters;
    State state = State::Idle;
    Triangle command;
    Point origin{};
    Bounds window{};
    int x = 0;
    int y = 0;
    int offset_x = 0;
    int offset_y = 0;
    unsigned edge_term = 0;
    unsigned multiply_wait = 0;
    std::int64_t edge_a_product = 0;
    std::array<std::int32_t, 3> edge_row{};
    std::array<std::int32_t, 3> edge_current{};
    std::array<std::int32_t, 3> edge_step_x{};
    std::array<std::int32_t, 3> edge_step_y{};
    // z,r,g,b each have one current accumulator and a row-start register.
    std::array<Plane, 4> planes{};
    std::array<std::int64_t, 4> plane_row{};
    std::array<std::int64_t, 4> plane_current{};
    std::array<bool, 4> plane_enabled{};
    Pending pending;

    void require_idle() const {
        if (state != State::Idle || pending.valid) {
            throw std::logic_error("opaque tile command requires a drained pipeline");
        }
    }

    void banks(const BsramCycle& color, const BsramCycle& low, const BsramCycle& high) {
        color_bank.tick(color);
        low_bank.tick(low);
        high_bank.tick(high);
    }

    void quiet_edge() {
        banks({}, {}, {});
        multiplier.tick({0, 0, false, false});
        ++counters.cycles;
    }

    void write_word(std::uint16_t address, std::uint16_t color, std::uint32_t depth) {
        banks({true, address, color, false, 0, false},
              {true, address, static_cast<std::uint16_t>(depth), false, 0, false},
              {true, address, static_cast<std::uint16_t>(depth >> 16), false, 0, false});
        multiplier.tick({0, 0, false, false});
        ++counters.cycles;
        ++counters.load_words;
    }

    void start_offsets() {
        if (offset_y != 0) state = State::OffsetY;
        else if (offset_x != 0) state = State::OffsetX;
        else {
            plane_current = plane_row;
            state = State::Raster;
        }
    }

    void advance_candidate() {
        if (x + 1 < window.x1) {
            ++x;
            for (std::size_t i = 0; i < 3; ++i) {
                edge_current[i] = add32(edge_current[i], edge_step_x[i]);
            }
            for (std::size_t i = 0; i < 4; ++i) {
                if (plane_enabled[i]) {
                    plane_current[i] = add64(plane_current[i], planes[i].step_x);
                }
            }
        } else if (y + 1 < window.y1) {
            x = window.x0;
            ++y;
            for (std::size_t i = 0; i < 3; ++i) {
                edge_row[i] = add32(edge_row[i], edge_step_y[i]);
            }
            edge_current = edge_row;
            for (std::size_t i = 0; i < 4; ++i) {
                if (plane_enabled[i]) plane_row[i] = add64(plane_row[i], planes[i].step_y);
            }
            plane_current = plane_row;
        } else {
            // Do not evaluate the accumulator one sample beyond the preflighted
            // domain. The final covered fragment still drains on a later edge.
            state = State::Drain;
        }
    }
};

OpaqueTile::OpaqueTile() : impl_(std::make_unique<Impl>()) {
    check_resource_capacity(OpaqueSliceResources::bsram_blocks,
                            OpaqueSliceResources::mult18_equivalents);
    reset();
}
OpaqueTile::~OpaqueTile() = default;
OpaqueTile::OpaqueTile(OpaqueTile&&) noexcept = default;
OpaqueTile& OpaqueTile::operator=(OpaqueTile&&) noexcept = default;

void OpaqueTile::reset() {
    auto& p = *impl_;
    p.state = Impl::State::Idle;
    p.pending = {};
    p.counters = {};
    const BsramCycle reset_output{false, 0, 0, false, 0, true};
    p.banks(reset_output, reset_output, reset_output);
    p.multiplier.tick({0, 0, false, true});
    ++p.counters.cycles;
}

void OpaqueTile::load(const TileImage& image) {
    auto& p = *impl_;
    p.require_idle();
    for (const auto value : image.depth) require_depth(value);
    for (std::size_t word = 0; word < tile_words; ++word) {
        p.write_word(static_cast<std::uint16_t>(word), image.color[word], image.depth[word]);
    }
}

void OpaqueTile::clear(std::uint16_t color, std::uint32_t depth) {
    auto& p = *impl_;
    p.require_idle();
    require_depth(depth);
    for (std::size_t word = 0; word < tile_words; ++word) {
        p.write_word(static_cast<std::uint16_t>(word), color, depth);
    }
}

TileImage OpaqueTile::store() {
    auto& p = *impl_;
    p.require_idle();
    TileImage result;
    for (std::size_t beat = 0; beat <= tile_words; ++beat) {
        // The downstream store adapter samples the old registered B outputs.
        if (beat != 0) {
            result.color[beat - 1] = p.color_bank.output();
            result.depth[beat - 1] = p.low_bank.output() |
                (static_cast<std::uint32_t>(p.high_bank.output()) << 16);
            ++p.counters.store_words;
        }
        const BsramCycle read{false, 0, 0, beat < tile_words,
                             static_cast<std::uint16_t>(beat < tile_words ? beat : 0), false};
        p.banks(read, read, read);
        p.multiplier.tick({0, 0, false, false});
        ++p.counters.cycles;
    }
    return result;
}

void OpaqueTile::begin(const Triangle& triangle, Point tile_origin) {
    auto& p = *impl_;
    p.require_idle();
    require_origin(tile_origin);
    require_opaque(triangle);
    const Bounds tile{tile_origin.x, tile_origin.y,
                      tile_origin.x + tile_size, tile_origin.y + tile_size};
    const Bounds window = intersect(triangle.bounds, tile);
    p.command = triangle;
    p.origin = tile_origin;
    p.window = window;
    p.pending = {};
    p.quiet_edge(); // command/parameter register capture
    ++p.counters.setup_cycles;
    if (window.empty()) return;

    p.x = window.x0;
    p.y = window.y0;
    p.offset_x = window.x0 - triangle.bounds.x0;
    p.offset_y = window.y0 - triangle.bounds.y0;
    p.edge_term = 0;
    p.multiply_wait = 0;
    p.planes = {triangle.z, triangle.r, triangle.g, triangle.b};
    const bool colors = triangle.state.color_write && triangle.state.shade == ShadeMode::Gouraud;
    p.plane_enabled = {triangle.state.depth_test || triangle.state.depth_write,
                       colors, colors, colors};
    for (std::size_t i = 0; i < 4; ++i) {
        p.plane_row[i] = p.plane_enabled[i] ? p.planes[i].seed : 0;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        // Multiplication by the fixed subpixel scale is wiring, with a wider
        // host check to catch out-of-profile coefficients before narrowing.
        p.edge_step_x[i] = narrow32(static_cast<std::int64_t>(triangle.edges[i].a) * 16);
        p.edge_step_y[i] = narrow32(static_cast<std::int64_t>(triangle.edges[i].b) * 16);
    }
    p.state = Impl::State::EdgeLaunch;
}

bool OpaqueTile::busy() const { return impl_->state != Impl::State::Idle; }
const TileStatistics& OpaqueTile::statistics() const { return impl_->counters; }

void OpaqueTile::tick(bool enable) {
    auto& p = *impl_;
    if (!enable) {
        p.quiet_edge();
        ++p.counters.stalls;
        return;
    }
    ++p.counters.cycles;
    BsramCycle color_port, low_port, high_port;
    Mult18Cycle multiply_port{0, 0, false, false};

    // Commit decisions use the previous edge's BSRAM output, never a value
    // produced later in this tick. The pending descriptor has exactly one slot.
    bool depth_write = false;
    std::uint16_t write_address = 0;
    std::uint32_t write_depth = 0;
    if (p.pending.valid) {
        const auto& fragment = p.pending;
        const std::uint32_t previous = fragment.depth_test
            ? (fragment.forwarded ? fragment.forwarded_depth
               : p.low_bank.output() | (static_cast<std::uint32_t>(p.high_bank.output()) << 16))
            : 0;
        const bool accepted = !fragment.depth_test ||
            depth_accept(fragment.function, fragment.depth, previous);
        if (!accepted) ++p.counters.depth_rejected;
        else {
            if (fragment.color_write) {
                color_port.write_enable = true;
                color_port.write_address = fragment.address;
                color_port.write_data = fragment.color;
                ++p.counters.color_writes;
            }
            if (fragment.depth_write) {
                depth_write = true;
                write_address = fragment.address;
                write_depth = fragment.depth;
                low_port.write_enable = high_port.write_enable = true;
                low_port.write_address = high_port.write_address = write_address;
                low_port.write_data = static_cast<std::uint16_t>(write_depth);
                high_port.write_data = static_cast<std::uint16_t>(write_depth >> 16);
                ++p.counters.depth_writes;
            }
        }
        p.pending = {};
    }

    switch (p.state) {
    case Impl::State::Idle:
        break;
    case Impl::State::EdgeLaunch: {
        ++p.counters.setup_cycles;
        const auto index = p.edge_term >> 1;
        const bool x_term = (p.edge_term & 1u) == 0;
        multiply_port = {x_term ? p.command.edges[index].a : p.command.edges[index].b,
                         16 * (x_term ? p.window.x0 : p.window.y0) + 8, true, false};
        p.multiply_wait = Mult18x18::enabled_edge_latency - 1;
        p.state = Impl::State::EdgeWait;
        break;
    }
    case Impl::State::EdgeWait:
        ++p.counters.setup_cycles;
        multiply_port.ce = true;
        if (--p.multiply_wait == 0) p.state = Impl::State::EdgeCapture;
        break;
    case Impl::State::EdgeCapture: {
        ++p.counters.setup_cycles;
        if (!p.multiplier.output_known()) throw std::logic_error("MULT18X18 result is unknown");
        if ((p.edge_term & 1u) == 0) p.edge_a_product = p.multiplier.product();
        else {
            const auto index = p.edge_term >> 1;
            p.edge_row[index] = narrow32(p.edge_a_product + p.multiplier.product() +
                                         p.command.edges[index].c);
            p.edge_current[index] = p.edge_row[index];
        }
        if (++p.edge_term == 6) p.start_offsets();
        else p.state = Impl::State::EdgeLaunch;
        break;
    }
    case Impl::State::OffsetY:
        ++p.counters.setup_cycles;
        for (std::size_t i = 0; i < 4; ++i) {
            if (p.plane_enabled[i]) p.plane_row[i] = add64(p.plane_row[i], p.planes[i].step_y);
        }
        if (--p.offset_y == 0) p.start_offsets();
        break;
    case Impl::State::OffsetX:
        ++p.counters.setup_cycles;
        for (std::size_t i = 0; i < 4; ++i) {
            if (p.plane_enabled[i]) p.plane_row[i] = add64(p.plane_row[i], p.planes[i].step_x);
        }
        if (--p.offset_x == 0) p.start_offsets();
        break;
    case Impl::State::Raster: {
        ++p.counters.candidates;
        bool inside = true;
        for (std::size_t i = 0; i < 3; ++i) {
            inside = inside && (p.edge_current[i] > 0 ||
                (p.edge_current[i] == 0 && p.command.edges[i].inclusive));
        }
        if (inside) {
            ++p.counters.covered_fragments;
            const auto& state = p.command.state;
            auto& fragment = p.pending;
            fragment.valid = true;
            fragment.address = static_cast<std::uint16_t>(((p.y - p.origin.y) << 5) |
                                                          (p.x - p.origin.x));
            fragment.depth = p.plane_enabled[0] ? quantize_q16(p.plane_current[0], depth_max) : 0;
            fragment.color_write = state.color_write;
            fragment.depth_write = state.depth_write;
            fragment.depth_test = state.depth_test;
            fragment.function = state.depth_func;
            Color color = state.color;
            if (state.color_write && state.shade == ShadeMode::Gouraud) {
                color = {static_cast<std::uint8_t>(quantize_q16(p.plane_current[1], 255)),
                         static_cast<std::uint8_t>(quantize_q16(p.plane_current[2], 255)),
                         static_cast<std::uint8_t>(quantize_q16(p.plane_current[3], 255))};
            }
            fragment.color = pack_color(color);
            if (state.depth_test) {
                if (depth_write && fragment.address == write_address) {
                    // The native SDP disallows this read/write collision. Gate
                    // both physical read ports and forward the new depth instead.
                    fragment.forwarded = true;
                    fragment.forwarded_depth = write_depth;
                    ++p.counters.forwarded_reads;
                } else {
                    low_port.read_enable = high_port.read_enable = true;
                    low_port.read_address = high_port.read_address = fragment.address;
                }
            }
        }
        p.advance_candidate();
        break;
    }
    case Impl::State::Drain:
        p.state = Impl::State::Idle;
        break;
    }
    p.banks(color_port, low_port, high_port);
    p.multiplier.tick(multiply_port);
}

void render_opaque_pass(const Pass& pass, std::vector<std::uint16_t>& color,
                        std::vector<std::uint32_t>& depth, Statistics& statistics) {
    if (color.size() != frame_words || depth.size() != frame_words) {
        throw std::invalid_argument("opaque pass attachments must be 640x480");
    }
    if ((pass.color_load != LoadOp::Load && pass.color_load != LoadOp::Clear) ||
        (pass.depth_load != LoadOp::Load && pass.depth_load != LoadOp::Clear)) {
        throw std::invalid_argument("unsupported opaque pass load operation");
    }
    require_depth(pass.clear_depth);
    if (pass.depth_load == LoadOp::Load) {
        for (const auto value : depth) require_depth(value);
    }
    for (const auto& primitive : pass.primitives) {
        const auto* triangle = std::get_if<Triangle>(&primitive);
        if (!triangle) throw std::invalid_argument("hardware opaque pass accepts triangles only");
        require_opaque(*triangle);
    }
    const auto bins = bin_primitives(pass.primitives);
    statistics = {};
    for (const auto& bin : bins) statistics.tile_references += bin.size();
    OpaqueTile slice;
    for (int tile = 0; tile < tile_count; ++tile) {
        const Point origin{(tile % tile_columns) * tile_size, (tile / tile_columns) * tile_size};
        TileImage input;
        for (int y = 0; y < tile_size; ++y) {
            for (int x = 0; x < tile_size; ++x) {
                const auto local = static_cast<std::size_t>((y << 5) | x);
                const auto global = static_cast<std::size_t>(origin.y + y) * screen_width + origin.x + x;
                input.color[local] = pass.color_load == LoadOp::Clear ? pass.clear_color : color[global];
                input.depth[local] = pass.depth_load == LoadOp::Clear ? pass.clear_depth : depth[global];
            }
        }
        slice.load(input);
        for (const auto reference : bins[static_cast<std::size_t>(tile)]) {
            slice.begin(std::get<Triangle>(pass.primitives[reference]), origin);
            while (slice.busy()) slice.tick();
        }
        const auto output = slice.store();
        for (int y = 0; y < tile_size; ++y) {
            for (int x = 0; x < tile_size; ++x) {
                const auto local = static_cast<std::size_t>((y << 5) | x);
                const auto global = static_cast<std::size_t>(origin.y + y) * screen_width + origin.x + x;
                color[global] = output.color[local];
                depth[global] = output.depth[local];
            }
        }
        ++statistics.tiles_processed;
    }
    const auto& counters = slice.statistics();
    statistics.candidates = counters.candidates;
    statistics.covered_fragments = counters.covered_fragments;
    statistics.depth_rejected = counters.depth_rejected;
    statistics.color_writes = counters.color_writes;
    statistics.depth_writes = counters.depth_writes;
    statistics.hardware_cycles = counters.cycles;
    statistics.hardware_stalls = counters.stalls;
    statistics.hardware_setup_cycles = counters.setup_cycles;
    statistics.hardware_forwarded_reads = counters.forwarded_reads;
}

} // namespace nano_gpu::hw
