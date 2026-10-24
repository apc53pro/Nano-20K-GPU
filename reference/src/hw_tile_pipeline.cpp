#include "nano_gpu/hw/tile_pipeline.hpp"

#include "nano_gpu/hw/bsram.hpp"
#include "nano_gpu/hw/device.hpp"
#include "nano_gpu/hw/mult18x18.hpp"
#include "nano_gpu/hw/perspective.hpp"
#include "nano_gpu/hw/sdram.hpp"
#include "nano_gpu/hw/texture.hpp"
#include "nano_gpu/hw/walker2d.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nano_gpu::hw {
namespace {

constexpr std::size_t tile_words = tile_size * tile_size;
constexpr std::size_t frame_words = screen_width * screen_height;

std::int64_t add64(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
        throw std::overflow_error("hardware signed 64-bit adder overflow");
    return a + b;
}
std::int32_t narrow32(std::int64_t value) {
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max())
        throw std::overflow_error("hardware signed 32-bit edge overflow");
    return static_cast<std::int32_t>(value);
}
std::int32_t add32(std::int32_t a, std::int32_t b) {
    return narrow32(static_cast<std::int64_t>(a) + b);
}
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
Color expand_color(std::uint16_t value) {
    const auto r = static_cast<std::uint8_t>((value >> 11) & 31u);
    const auto g = static_cast<std::uint8_t>((value >> 5) & 63u);
    const auto b = static_cast<std::uint8_t>(value & 31u);
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)),
            static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2))};
}
bool textured(ShadeMode mode) {
    return mode == ShadeMode::TextureReplace || mode == ShadeMode::TextureFlatModulate ||
           mode == ShadeMode::TextureGouraudModulate;
}
bool modulated(ShadeMode mode) {
    return mode == ShadeMode::TextureFlatModulate || mode == ShadeMode::TextureGouraudModulate;
}
int floor_coordinate(std::int32_t q16) {
    if (q16 >= 0) return q16 >> 16;
    return -static_cast<int>((-static_cast<std::int64_t>(q16) + 65535) >> 16);
}
int address_coordinate(std::int32_t q16, int size, AddressMode mode) {
    const int integer = floor_coordinate(q16);
    if (mode == AddressMode::Clamp) return std::clamp(integer, 0, size - 1);
    if (mode == AddressMode::Repeat)
        return static_cast<int>(static_cast<std::uint32_t>(integer) & static_cast<unsigned>(size - 1));
    throw std::invalid_argument("unsupported hardware texture address mode");
}
bool depth_accept(DepthFunc function, std::uint32_t incoming, std::uint32_t stored) {
    switch (function) {
    case DepthFunc::Less: return incoming < stored;
    case DepthFunc::LessEqual: return incoming <= stored;
    case DepthFunc::Always: return true;
    }
    throw std::invalid_argument("unsupported hardware depth comparison");
}
void require_depth(std::uint32_t value) {
    if (value > depth_max) throw std::out_of_range("hardware depth exceeds D24");
}
void require_origin(Point origin) {
    if (origin.x < 0 || origin.x >= screen_width || origin.x % tile_size != 0 ||
        origin.y < 0 || origin.y >= screen_height || origin.y % tile_size != 0)
        throw std::invalid_argument("hardware tile origin must name a visible 32x32 tile");
}
void validate_record(const Primitive& primitive) {
    if (const auto* triangle = std::get_if<Triangle>(&primitive)) validate_triangle(*triangle);
    else {
        // CPU-side format validation is outside the GPU datapath. The walker
        // validates the whole source rectangle even when this tile is empty.
        Walker2D validator;
        validator.begin(primitive, {0, 0});
    }
}
std::uint8_t channel(Color color, unsigned index) {
    return index == 0 ? color.r : index == 1 ? color.g : color.b;
}
void set_channel(Color& color, unsigned index, std::uint8_t value) {
    if (index == 0) color.r = value;
    else if (index == 1) color.g = value;
    else color.b = value;
}

} // namespace

const char* fragment_stage_name(FragmentStage stage) {
    switch (stage) {
    case FragmentStage::Idle: return "idle";
    case FragmentStage::Transfer: return "transfer";
    case FragmentStage::Setup: return "setup";
    case FragmentStage::Raster: return "raster";
    case FragmentStage::Depth: return "depth";
    case FragmentStage::PerspectiveU: return "perspective_u";
    case FragmentStage::PerspectiveV: return "perspective_v";
    case FragmentStage::Texture: return "texture";
    case FragmentStage::Shade: return "shade";
    case FragmentStage::Commit: return "commit";
    case FragmentStage::Drain: return "drain";
    case FragmentStage::Reset: return "reset";
    }
    throw std::invalid_argument("unknown fragment stage");
}

const char* fragment_retirement_name(FragmentRetirement retirement) {
    switch (retirement) {
    case FragmentRetirement::None: return "none";
    case FragmentRetirement::Commit: return "commit";
    case FragmentRetirement::DepthRejected: return "depth_rejected";
    case FragmentRetirement::KeyRejected: return "key_rejected";
    case FragmentRetirement::Cancelled: return "cancelled";
    }
    throw std::invalid_argument("unknown fragment retirement");
}

struct OpaqueTile::Impl {
    enum class State {
        Idle, EdgeLaunch, EdgeWait, EdgeCapture, OffsetY, OffsetX, Raster, Drain,
        FragmentDepth, PerspectiveUStart, PerspectiveUWait, PerspectiveUCapture,
        PerspectiveVStart, PerspectiveVWait, PerspectiveVCapture, TextureStart,
        TextureWait, TextureCapture, ModLaunch, ModWait, ModCapture, FragmentCommit
    };
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
    struct Held {
        std::uint16_t address = 0;
        std::uint32_t depth = 0;
        Color factor{}, color{};
        std::int64_t q = 0, u = 0, v = 0;
        std::int32_t recovered_u = 0, recovered_v = 0;
        int source_x = 0, source_y = 0;
        unsigned mod_channel = 0;
    };

    Sdp1kx16 color_bank, low_bank, high_bank;
    Mult18x18 multiplier;
    PerspectiveDivider perspective;
    TextureUnit texture;
    Walker2D walker;
    TileStatistics counters;
    State state = State::Idle;
    Triangle triangle;
    RenderState draw_state;
    bool triangle_draw = true;
    bool sprite_draw = false;
    bool need_texture = false;
    std::uint32_t constant_depth = 0;
    Point origin{};
    Bounds window{};
    int x = 0, y = 0, offset_x = 0, offset_y = 0;
    unsigned edge_term = 0, multiply_wait = 0;
    std::int64_t edge_a_product = 0;
    std::array<std::int32_t, 3> edge_row{}, edge_current{}, edge_step_x{}, edge_step_y{};
    // z,r,g,b,q,U,V are distinct bounded hardware accumulator registers.
    std::array<Plane, 7> planes{};
    std::array<std::int64_t, 7> plane_row{}, plane_current{};
    std::array<bool, 7> plane_enabled{};
    Pending pending;
    Held held;
    bool transfer_read_valid = false;

    // Observer bookkeeping only. The real pending/held datapath above retains
    // one fragment; no extra hardware queue or fragment-context RAM is added.
    FragmentCycle event;
    FragmentCycleObserver observer;
    FragmentToken observed_fragment;
    std::uint64_t next_fragment_id = 1, primitive_id = 0;

    FragmentStage current_stage() const {
        switch (state) {
        case State::Idle: return FragmentStage::Idle;
        case State::EdgeLaunch: case State::EdgeWait: case State::EdgeCapture:
        case State::OffsetY: case State::OffsetX: return FragmentStage::Setup;
        case State::Raster: return FragmentStage::Raster;
        case State::Drain: return FragmentStage::Drain;
        case State::FragmentDepth: return FragmentStage::Depth;
        case State::PerspectiveUStart: case State::PerspectiveUWait:
        case State::PerspectiveUCapture: return FragmentStage::PerspectiveU;
        case State::PerspectiveVStart: case State::PerspectiveVWait:
        case State::PerspectiveVCapture: return FragmentStage::PerspectiveV;
        case State::TextureStart: case State::TextureWait:
        case State::TextureCapture: return FragmentStage::Texture;
        case State::ModLaunch: case State::ModWait:
        case State::ModCapture: return FragmentStage::Shade;
        case State::FragmentCommit: return FragmentStage::Commit;
        }
        throw std::logic_error("unknown tile controller state");
    }
    void start_event(FragmentStage stage, bool enabled = true) {
        event = {};
        event.stage = stage;
        event.enabled = enabled;
        event.inflight_before = observed_fragment.id != 0 ? 1u : 0u;
    }
    void finish_event() {
        event.cycle = counters.cycles;
        event.inflight_after = observed_fragment.id != 0 ? 1u : 0u;
        if (observer) observer(event);
    }
    void accept_fragment(int px, int py) {
        if (observed_fragment.id != 0 || event.accepted)
            throw std::logic_error("fragment observer detected an overwritten context");
        if (next_fragment_id == 0)
            throw std::overflow_error("fragment observer ID space exhausted");
        observed_fragment = {next_fragment_id++, primitive_id, px, py,
                             local_address(px, py), need_texture};
        event.accepted = true;
        event.accepted_fragment = observed_fragment;
    }
    void retire_fragment(FragmentRetirement reason, bool color = false, bool depth = false) {
        if (observed_fragment.id == 0 || event.retired)
            throw std::logic_error("fragment observer detected retirement without acceptance");
        event.retired = true;
        event.retired_fragment = observed_fragment;
        event.retirement = reason;
        event.color_write = color;
        event.depth_write = depth;
        observed_fragment = {};
    }

    void require_idle() const {
        if (state != State::Idle || pending.valid)
            throw std::logic_error("hardware tile command requires a drained pipeline");
    }
    void banks(const BsramCycle& color, const BsramCycle& low, const BsramCycle& high) {
        color_bank.tick(color); low_bank.tick(low); high_bank.tick(high);
    }
    void quiet_edge(FragmentStage stage = FragmentStage::Idle, bool enabled = true) {
        start_event(stage, enabled);
        banks({}, {}, {});
        multiplier.tick({0, 0, false, false});
        ++counters.cycles;
        finish_event();
    }
    void write_word(std::uint16_t address, std::uint16_t color, std::uint32_t depth) {
        start_event(FragmentStage::Transfer);
        banks({true, address, color, false, 0, false},
              {true, address, static_cast<std::uint16_t>(depth), false, 0, false},
              {true, address, static_cast<std::uint16_t>(depth >> 16), false, 0, false});
        multiplier.tick({0, 0, false, false});
        ++counters.cycles; ++counters.load_words;
        finish_event();
    }
    void start_offsets() {
        if (offset_y != 0) state = State::OffsetY;
        else if (offset_x != 0) state = State::OffsetX;
        else { plane_current = plane_row; state = State::Raster; }
    }
    void advance_candidate() {
        if (!triangle_draw) {
            walker.tick();
            state = walker.busy() ? State::Raster : State::Drain;
            return;
        }
        if (x + 1 < window.x1) {
            ++x;
            for (std::size_t i = 0; i < 3; ++i)
                edge_current[i] = add32(edge_current[i], edge_step_x[i]);
            for (std::size_t i = 0; i < 7; ++i)
                if (plane_enabled[i]) plane_current[i] = add64(plane_current[i], planes[i].step_x);
            state = State::Raster;
        } else if (y + 1 < window.y1) {
            x = window.x0; ++y;
            for (std::size_t i = 0; i < 3; ++i) edge_row[i] = add32(edge_row[i], edge_step_y[i]);
            edge_current = edge_row;
            for (std::size_t i = 0; i < 7; ++i)
                if (plane_enabled[i]) plane_row[i] = add64(plane_row[i], planes[i].step_y);
            plane_current = plane_row;
            state = State::Raster;
        } else state = State::Drain;
    }
    std::uint16_t local_address(int px, int py) const {
        return static_cast<std::uint16_t>(((py - origin.y) << 5) | (px - origin.x));
    }
    Color sample_factor() const {
        if (triangle_draw && draw_state.color_write &&
            (draw_state.shade == ShadeMode::Gouraud || draw_state.shade == ShadeMode::TextureGouraudModulate))
            return {static_cast<std::uint8_t>(quantize_q16(plane_current[1], 255)),
                    static_cast<std::uint8_t>(quantize_q16(plane_current[2], 255)),
                    static_cast<std::uint8_t>(quantize_q16(plane_current[3], 255))};
        return draw_state.color;
    }
    std::uint32_t sample_depth() const {
        if (!(draw_state.depth_test || draw_state.depth_write)) return 0;
        return triangle_draw ? quantize_q16(plane_current[0], depth_max) : constant_depth;
    }
    void after_depth() {
        state = sprite_draw ? State::TextureStart : State::PerspectiveUStart;
    }
    void sync_texture_statistics() {
        const auto& t = texture.statistics();
        counters.texture_hits = t.hits;
        counters.texture_misses = t.misses;
        counters.texture_refill_words = t.refill_words;
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
    // An accepted external request must be retired before its consumer resets.
    // Texture reset diagnoses this before any other tile state is changed.
    p.texture.reset();
    p.start_event(FragmentStage::Reset);
    if (p.observed_fragment.id != 0) p.retire_fragment(FragmentRetirement::Cancelled);
    p.state = Impl::State::Idle; p.pending = {}; p.held = {}; p.counters = {};
    p.transfer_read_valid = false;
    const BsramCycle reset_output{false, 0, 0, false, 0, true};
    p.banks(reset_output, reset_output, reset_output);
    p.multiplier.tick({0, 0, false, true});
    p.perspective.reset();
    ++p.counters.cycles;
    p.finish_event();
}
void OpaqueTile::load(const TileImage& image) {
    auto& p = *impl_; p.require_idle();
    for (const auto value : image.depth) require_depth(value);
    for (std::size_t word = 0; word < tile_words; ++word)
        p.write_word(static_cast<std::uint16_t>(word), image.color[word], image.depth[word]);
}
void OpaqueTile::clear(std::uint16_t color, std::uint32_t depth) {
    auto& p = *impl_; p.require_idle(); require_depth(depth);
    for (std::size_t word = 0; word < tile_words; ++word)
        p.write_word(static_cast<std::uint16_t>(word), color, depth);
}
TileImage OpaqueTile::store() {
    auto& p = *impl_; p.require_idle(); TileImage result;
    for (std::size_t beat = 0; beat <= tile_words; ++beat) {
        p.start_event(FragmentStage::Transfer);
        if (beat != 0) {
            result.color[beat - 1] = p.color_bank.output();
            result.depth[beat - 1] = p.low_bank.output() |
                (static_cast<std::uint32_t>(p.high_bank.output()) << 16);
            ++p.counters.store_words;
        }
        const BsramCycle read{false, 0, 0, beat < tile_words,
                             static_cast<std::uint16_t>(beat < tile_words ? beat : 0), false};
        p.banks(read, read, read);
        p.multiplier.tick({0, 0, false, false}); ++p.counters.cycles;
        p.finish_event();
    }
    return result;
}
void OpaqueTile::load_word(std::uint16_t address, std::uint16_t color, std::uint32_t depth) {
    auto& p = *impl_; p.require_idle(); require_depth(depth);
    if (address >= tile_words) throw std::out_of_range("tile DMA word address exceeds ten bits");
    p.transfer_read_valid = false;
    p.write_word(address, color, depth);
}
void OpaqueTile::read_word_request(std::uint16_t address) {
    auto& p = *impl_; p.require_idle();
    if (address >= tile_words) throw std::out_of_range("tile DMA word address exceeds ten bits");
    p.start_event(FragmentStage::Transfer);
    const BsramCycle read{false, 0, 0, true, address, false};
    p.banks(read, read, read);
    p.multiplier.tick({0, 0, false, false});
    p.transfer_read_valid = true;
    ++p.counters.cycles;
    ++p.counters.store_words;
    p.finish_event();
}
TileWord OpaqueTile::read_word_result() const {
    const auto& p = *impl_; p.require_idle();
    if (!p.transfer_read_valid) throw std::logic_error("tile DMA result has no preceding read request");
    return {p.color_bank.output(), p.low_bank.output() |
            (static_cast<std::uint32_t>(p.high_bank.output()) << 16)};
}
void OpaqueTile::idle_edge() {
    auto& p = *impl_; p.require_idle(); p.quiet_edge();
}
void OpaqueTile::invalidate_texture() {
    auto& p = *impl_; p.require_idle();
    p.texture.invalidate();
    ++p.counters.texture_cycles;
    p.quiet_edge(FragmentStage::Transfer);
}
void OpaqueTile::copy_buffer_write(std::uint16_t address, std::uint16_t value) {
    auto& p = *impl_; p.require_idle();
    p.texture.scratch_write(address, value);
    p.quiet_edge(FragmentStage::Transfer);
}
void OpaqueTile::copy_buffer_read_request(std::uint16_t address) {
    auto& p = *impl_; p.require_idle();
    p.texture.scratch_read_request(address);
    p.quiet_edge(FragmentStage::Transfer);
}
std::uint16_t OpaqueTile::copy_buffer_read_result() const {
    const auto& p = *impl_; p.require_idle();
    return p.texture.scratch_read_result();
}
void OpaqueTile::begin(const Triangle& value, Point tile_origin) {
    begin(Primitive{value}, tile_origin);
}
void OpaqueTile::begin(const Primitive& primitive, Point tile_origin) {
    begin_impl(primitive, tile_origin, std::nullopt, nullptr);
}
void OpaqueTile::begin_external(const Primitive& primitive, Point tile_origin,
                                const std::optional<TextureDescriptor>& texture,
                                SdramController& memory) {
    begin_impl(primitive, tile_origin, texture, &memory);
}
void OpaqueTile::begin_impl(const Primitive& primitive, Point tile_origin,
                           const std::optional<TextureDescriptor>& texture,
                           SdramController* memory) {
    auto& p = *impl_; p.require_idle(); require_origin(tile_origin); validate_record(primitive);
    const auto state = std::visit([](const auto& item) { return item.state; }, primitive);
    const bool triangle_draw = std::holds_alternative<Triangle>(primitive);
    const bool sprite_draw = std::holds_alternative<Sprite>(primitive);
    const bool need_texture = (sprite_draw || (triangle_draw && textured(state.shade))) &&
                              (state.color_write || state.color_key.has_value());
    if (need_texture && memory) {
        if (!texture || !state.texture || texture->width != static_cast<unsigned>(state.texture->width()) ||
            texture->height != static_cast<unsigned>(state.texture->height()) ||
            texture->pitch_bytes != static_cast<unsigned>(state.texture->pitch_bytes()))
            throw std::invalid_argument("fetched texture descriptor does not match primitive metadata");
        p.texture.bind_external(*texture, *memory);
    } else if (need_texture) p.texture.bind(state.texture);
    p.origin = tile_origin; p.pending = {}; p.held = {};
    p.transfer_read_valid = false;
    p.triangle_draw = triangle_draw;
    p.sprite_draw = sprite_draw;
    p.draw_state = state;
    p.need_texture = need_texture;
    if (++p.primitive_id == 0)
        throw std::overflow_error("fragment observer primitive ID space exhausted");
    if (p.need_texture) ++p.counters.texture_cycles;
    ++p.counters.setup_cycles;
    p.quiet_edge(FragmentStage::Setup);
    if (!p.triangle_draw) {
        p.constant_depth = std::visit([](const auto& item) -> std::uint32_t {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, Triangle>) return 0;
            else return item.depth;
        }, primitive);
        p.walker.begin(primitive, tile_origin);
        p.state = p.walker.busy() ? Impl::State::Raster : Impl::State::Idle;
        return;
    }
    p.triangle = std::get<Triangle>(primitive);
    const Bounds tile{tile_origin.x, tile_origin.y, tile_origin.x + tile_size, tile_origin.y + tile_size};
    p.window = intersect(p.triangle.bounds, tile);
    if (p.window.empty()) { p.state = Impl::State::Idle; return; }
    p.x = p.window.x0; p.y = p.window.y0;
    p.offset_x = p.window.x0 - p.triangle.bounds.x0;
    p.offset_y = p.window.y0 - p.triangle.bounds.y0;
    p.edge_term = 0; p.multiply_wait = 0;
    p.planes = {p.triangle.z, p.triangle.r, p.triangle.g, p.triangle.b,
                p.triangle.q, p.triangle.u, p.triangle.v};
    const bool colors = p.draw_state.color_write &&
        (p.draw_state.shade == ShadeMode::Gouraud || p.draw_state.shade == ShadeMode::TextureGouraudModulate);
    p.plane_enabled = {p.draw_state.depth_test || p.draw_state.depth_write, colors, colors, colors,
                       p.need_texture, p.need_texture, p.need_texture};
    for (std::size_t i = 0; i < 7; ++i) p.plane_row[i] = p.plane_enabled[i] ? p.planes[i].seed : 0;
    for (std::size_t i = 0; i < 3; ++i) {
        p.edge_step_x[i] = narrow32(static_cast<std::int64_t>(p.triangle.edges[i].a) * 16);
        p.edge_step_y[i] = narrow32(static_cast<std::int64_t>(p.triangle.edges[i].b) * 16);
    }
    p.state = Impl::State::EdgeLaunch;
}
bool OpaqueTile::busy() const { return impl_->state != Impl::State::Idle; }
bool OpaqueTile::external_memory_pending() const { return impl_->texture.external_memory_pending(); }
const TileStatistics& OpaqueTile::statistics() const { return impl_->counters; }
const FragmentCycle& OpaqueTile::last_cycle() const { return impl_->event; }
void OpaqueTile::set_cycle_observer(FragmentCycleObserver observer) { impl_->observer = std::move(observer); }

void OpaqueTile::tick(bool enable) {
    auto& p = *impl_;
    if (!enable) { ++p.counters.stalls; p.quiet_edge(p.current_stage(), false); return; }
    p.start_event(p.current_stage());
    ++p.counters.cycles;
    BsramCycle color_port, low_port, high_port;
    Mult18Cycle multiply_port{0, 0, false, false};
    bool depth_write = false;
    std::uint16_t write_address = 0;
    std::uint32_t write_depth = 0;
    if (p.pending.valid) {
        const auto& f = p.pending;
        const auto previous = f.depth_test ?
            (f.forwarded ? f.forwarded_depth : p.low_bank.output() |
             (static_cast<std::uint32_t>(p.high_bank.output()) << 16)) : 0;
        const bool accepted = !f.depth_test || depth_accept(f.function, f.depth, previous);
        p.retire_fragment(accepted ? FragmentRetirement::Commit : FragmentRetirement::DepthRejected,
                          accepted && f.color_write, accepted && f.depth_write);
        if (!accepted) ++p.counters.depth_rejected;
        else {
            if (f.color_write) {
                color_port = {true, f.address, f.color, false, 0, false};
                ++p.counters.color_writes;
            }
            if (f.depth_write) {
                depth_write = true; write_address = f.address; write_depth = f.depth;
                low_port = {true, f.address, static_cast<std::uint16_t>(f.depth), false, 0, false};
                high_port = {true, f.address, static_cast<std::uint16_t>(f.depth >> 16), false, 0, false};
                ++p.counters.depth_writes;
            }
        }
        p.pending = {};
    }
    using State = Impl::State;
    switch (p.state) {
    case State::Idle: break;
    case State::EdgeLaunch: {
        ++p.counters.setup_cycles;
        const auto index = p.edge_term >> 1;
        const bool x_term = (p.edge_term & 1u) == 0;
        multiply_port = {x_term ? p.triangle.edges[index].a : p.triangle.edges[index].b,
                         16 * (x_term ? p.window.x0 : p.window.y0) + 8, true, false};
        p.multiply_wait = Mult18x18::enabled_edge_latency - 1;
        p.state = State::EdgeWait; break;
    }
    case State::EdgeWait:
        ++p.counters.setup_cycles; multiply_port.ce = true;
        if (--p.multiply_wait == 0) p.state = State::EdgeCapture;
        break;
    case State::EdgeCapture: {
        ++p.counters.setup_cycles;
        if (!p.multiplier.output_known()) throw std::logic_error("MULT18X18 result is unknown");
        if ((p.edge_term & 1u) == 0) p.edge_a_product = p.multiplier.product();
        else {
            const auto index = p.edge_term >> 1;
            p.edge_row[index] = narrow32(p.edge_a_product + p.multiplier.product() + p.triangle.edges[index].c);
            p.edge_current[index] = p.edge_row[index];
        }
        if (++p.edge_term == 6) p.start_offsets(); else p.state = State::EdgeLaunch;
        break;
    }
    case State::OffsetY:
        ++p.counters.setup_cycles;
        for (std::size_t i = 0; i < 7; ++i)
            if (p.plane_enabled[i]) p.plane_row[i] = add64(p.plane_row[i], p.planes[i].step_y);
        if (--p.offset_y == 0) p.start_offsets();
        break;
    case State::OffsetX:
        ++p.counters.setup_cycles;
        for (std::size_t i = 0; i < 7; ++i)
            if (p.plane_enabled[i]) p.plane_row[i] = add64(p.plane_row[i], p.planes[i].step_x);
        if (--p.offset_x == 0) p.start_offsets();
        break;
    case State::Raster: {
        p.event.candidate = true;
        int px = p.x, py = p.y;
        bool inside = true;
        Sample2D point;
        if (p.triangle_draw) {
            ++p.counters.candidates;
            for (std::size_t i = 0; i < 3; ++i)
                inside = inside && (p.edge_current[i] > 0 ||
                    (p.edge_current[i] == 0 && p.triangle.edges[i].inclusive));
        } else {
            point = p.walker.sample(); px = point.x; py = point.y; inside = point.covered;
            if (inside) ++p.counters.candidates;
        }
        if (inside) {
            p.event.covered = true;
            p.accept_fragment(px, py);
            ++p.counters.covered_fragments;
            if (p.need_texture) {
                auto& h = p.held;
                h = {}; h.address = p.local_address(px, py); h.depth = p.sample_depth();
                h.factor = p.sample_factor();
                if (p.triangle_draw) { h.q = p.plane_current[4]; h.u = p.plane_current[5]; h.v = p.plane_current[6]; }
                else { h.source_x = point.source_x; h.source_y = point.source_y; }
                if (p.draw_state.depth_test) {
                    low_port.read_enable = high_port.read_enable = true;
                    low_port.read_address = high_port.read_address = h.address;
                    p.state = State::FragmentDepth;
                } else p.after_depth();
                break;
            }
            auto& f = p.pending;
            f.valid = true; f.address = p.local_address(px, py); f.depth = p.sample_depth();
            f.color = pack_color(p.sample_factor());
            f.color_write = p.draw_state.color_write; f.depth_write = p.draw_state.depth_write;
            f.depth_test = p.draw_state.depth_test; f.function = p.draw_state.depth_func;
            if (f.depth_test) {
                if (depth_write && f.address == write_address) {
                    f.forwarded = true; f.forwarded_depth = write_depth; ++p.counters.forwarded_reads;
                } else {
                    low_port.read_enable = high_port.read_enable = true;
                    low_port.read_address = high_port.read_address = f.address;
                }
            }
        }
        p.advance_candidate();
        break;
    }
    case State::Drain: p.state = State::Idle; break;
    case State::FragmentDepth: {
        const auto stored = p.low_bank.output() | (static_cast<std::uint32_t>(p.high_bank.output()) << 16);
        if (!depth_accept(p.draw_state.depth_func, p.held.depth, stored)) {
            p.retire_fragment(FragmentRetirement::DepthRejected);
            ++p.counters.depth_rejected; p.advance_candidate();
        } else p.after_depth();
        break;
    }
    case State::PerspectiveUStart:
        p.perspective.start(p.held.u, p.held.q); ++p.counters.perspective_cycles;
        p.state = State::PerspectiveUWait; break;
    case State::PerspectiveUWait:
        p.perspective.tick(); ++p.counters.perspective_cycles;
        if (!p.perspective.busy()) p.state = State::PerspectiveUCapture;
        break;
    case State::PerspectiveUCapture:
        // The consumer captures the divider's OLD registered output on this
        // edge. It cannot consume the result produced by the same edge above.
        if (!p.perspective.result_valid()) throw std::logic_error("perspective U result is invalid");
        p.held.recovered_u = p.perspective.result();
        p.state = State::PerspectiveVStart;
        break;
    case State::PerspectiveVStart:
        p.perspective.start(p.held.v, p.held.q); ++p.counters.perspective_cycles;
        p.state = State::PerspectiveVWait; break;
    case State::PerspectiveVWait:
        p.perspective.tick(); ++p.counters.perspective_cycles;
        if (!p.perspective.busy()) p.state = State::PerspectiveVCapture;
        break;
    case State::PerspectiveVCapture:
        if (!p.perspective.result_valid()) throw std::logic_error("perspective V result is invalid");
        p.held.recovered_v = p.perspective.result();
        p.state = State::TextureStart;
        break;
    case State::TextureStart:
        if (!p.sprite_draw) {
            p.held.source_x = address_coordinate(p.held.recovered_u, p.draw_state.texture->width(), p.draw_state.address_u);
            p.held.source_y = address_coordinate(p.held.recovered_v, p.draw_state.texture->height(), p.draw_state.address_v);
        }
        p.texture.start(p.held.source_x, p.held.source_y); ++p.counters.texture_cycles;
        p.state = State::TextureWait; break;
    case State::TextureWait:
        p.texture.tick(); ++p.counters.texture_cycles;
        if (!p.texture.busy()) p.state = State::TextureCapture;
        break;
    case State::TextureCapture:
        if (!p.texture.result_valid()) throw std::logic_error("texture result is invalid");
        {
            const auto texel = p.texture.result(); ++p.counters.texture_samples;
            p.sync_texture_statistics();
            if (p.draw_state.color_key && texel == *p.draw_state.color_key) {
                p.retire_fragment(FragmentRetirement::KeyRejected);
                ++p.counters.keyed_rejected; p.advance_candidate();
            } else {
                p.held.color = expand_color(texel);
                if (p.triangle_draw && p.draw_state.color_write && modulated(p.draw_state.shade)) {
                    p.held.mod_channel = 0; p.state = State::ModLaunch;
                } else p.state = State::FragmentCommit;
            }
        }
        break;
    case State::ModLaunch:
        multiply_port = {channel(p.held.color, p.held.mod_channel),
                         channel(p.held.factor, p.held.mod_channel), true, false};
        p.multiply_wait = Mult18x18::enabled_edge_latency - 1; p.state = State::ModWait;
        break;
    case State::ModWait:
        multiply_port.ce = true;
        if (--p.multiply_wait == 0) p.state = State::ModCapture;
        break;
    case State::ModCapture: {
        if (!p.multiplier.output_known()) throw std::logic_error("modulation product is unknown");
        const auto adjusted = static_cast<std::uint32_t>(p.multiplier.product()) + 128u;
        const auto result = static_cast<std::uint8_t>((adjusted + (adjusted >> 8)) >> 8);
        set_channel(p.held.color, p.held.mod_channel, result);
        if (++p.held.mod_channel == 3) p.state = State::FragmentCommit;
        else p.state = State::ModLaunch;
        break;
    }
    case State::FragmentCommit:
        p.retire_fragment(FragmentRetirement::Commit, p.draw_state.color_write, p.draw_state.depth_write);
        if (p.draw_state.color_write) {
            color_port = {true, p.held.address, pack_color(p.held.color), false, 0, false};
            ++p.counters.color_writes;
        }
        if (p.draw_state.depth_write) {
            low_port = {true, p.held.address, static_cast<std::uint16_t>(p.held.depth), false, 0, false};
            high_port = {true, p.held.address, static_cast<std::uint16_t>(p.held.depth >> 16), false, 0, false};
            ++p.counters.depth_writes;
        }
        p.advance_candidate();
        break;
    }
    p.banks(color_port, low_port, high_port);
    p.multiplier.tick(multiply_port);
    p.finish_event();
}

void render_pass(const Pass& pass, std::vector<std::uint16_t>& color,
                 std::vector<std::uint32_t>& depth, Statistics& statistics) {
    if (color.size() != frame_words || depth.size() != frame_words)
        throw std::invalid_argument("hardware pass attachments must be 640x480");
    if ((pass.color_load != LoadOp::Load && pass.color_load != LoadOp::Clear) ||
        (pass.depth_load != LoadOp::Load && pass.depth_load != LoadOp::Clear))
        throw std::invalid_argument("unsupported hardware pass load operation");
    require_depth(pass.clear_depth);
    if (pass.depth_load == LoadOp::Load) for (const auto value : depth) require_depth(value);
    for (const auto& primitive : pass.primitives) validate_record(primitive);
    const auto bins = bin_primitives(pass.primitives);
    statistics = {};
    for (const auto& bin : bins) statistics.tile_references += bin.size();
    OpaqueTile slice;
    for (int tile = 0; tile < tile_count; ++tile) {
        const Point origin{(tile % tile_columns) * tile_size, (tile / tile_columns) * tile_size};
        TileImage input;
        for (int y = 0; y < tile_size; ++y) for (int x = 0; x < tile_size; ++x) {
            const auto local = static_cast<std::size_t>((y << 5) | x);
            const auto global = static_cast<std::size_t>(origin.y + y) * screen_width + origin.x + x;
            input.color[local] = pass.color_load == LoadOp::Clear ? pass.clear_color : color[global];
            input.depth[local] = pass.depth_load == LoadOp::Clear ? pass.clear_depth : depth[global];
        }
        slice.load(input);
        for (const auto reference : bins[static_cast<std::size_t>(tile)]) {
            slice.begin(pass.primitives[reference], origin);
            while (slice.busy()) slice.tick();
        }
        const auto output = slice.store();
        for (int y = 0; y < tile_size; ++y) for (int x = 0; x < tile_size; ++x) {
            const auto local = static_cast<std::size_t>((y << 5) | x);
            const auto global = static_cast<std::size_t>(origin.y + y) * screen_width + origin.x + x;
            color[global] = output.color[local]; depth[global] = output.depth[local];
        }
        ++statistics.tiles_processed;
    }
    const auto& c = slice.statistics();
    statistics.candidates = c.candidates; statistics.covered_fragments = c.covered_fragments;
    statistics.depth_rejected = c.depth_rejected; statistics.keyed_rejected = c.keyed_rejected;
    statistics.texture_samples = c.texture_samples;
    statistics.color_writes = c.color_writes; statistics.depth_writes = c.depth_writes;
    statistics.hardware_cycles = c.cycles; statistics.hardware_stalls = c.stalls;
    statistics.hardware_setup_cycles = c.setup_cycles; statistics.hardware_forwarded_reads = c.forwarded_reads;
    statistics.hardware_perspective_cycles = c.perspective_cycles;
    statistics.hardware_texture_cycles = c.texture_cycles;
    statistics.hardware_texture_hits = c.texture_hits; statistics.hardware_texture_misses = c.texture_misses;
    statistics.hardware_texture_refill_words = c.texture_refill_words;
}
void render_opaque_pass(const Pass& pass, std::vector<std::uint16_t>& color,
                        std::vector<std::uint32_t>& depth, Statistics& statistics) {
    render_pass(pass, color, depth, statistics);
}

} // namespace nano_gpu::hw
