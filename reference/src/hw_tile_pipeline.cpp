#include "nano_gpu/hw/tile_pipeline.hpp"

#include "nano_gpu/hw/bsram.hpp"
#include "nano_gpu/hw/device.hpp"
#include "nano_gpu/hw/mult18x18.hpp"
#include "nano_gpu/hw/fragment_pipeline.hpp"
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
bool textured(ShadeMode mode) {
    return mode == ShadeMode::TextureReplace || mode == ShadeMode::TextureFlatModulate ||
           mode == ShadeMode::TextureGouraudModulate;
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
        Idle, EdgeLaunch, EdgeWait, EdgeCapture, OffsetY, OffsetX, Raster, Drain
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
    // These are fixed physical banks. A role-select mux chooses this array
    // or the one connected external set; the pointers model wiring/selection,
    // not movable RAM payloads or dynamically allocated hardware storage.
    AttachmentBanks owned_banks;
    AttachmentBanks* active_banks = &owned_banks;
    AttachmentBanks* connected_banks = nullptr;
    Sdp1kx16& color_bank() { return (*active_banks)[0]; }
    Sdp1kx16& low_bank() { return (*active_banks)[1]; }
    Sdp1kx16& high_bank() { return (*active_banks)[2]; }
    const Sdp1kx16& color_bank() const { return (*active_banks)[0]; }
    const Sdp1kx16& low_bank() const { return (*active_banks)[1]; }
    const Sdp1kx16& high_bank() const { return (*active_banks)[2]; }
    Mult18x18 multiplier;
    TextureUnit texture;
    TexturedFragmentPipeline fragments{texture};
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
    bool transfer_read_valid = false;

    // Observer identities are separate from the six physical context banks.
    // They never provide fragment data or additional hardware storage ports.
    FragmentCycle event;
    FragmentCycleObserver observer;
    FragmentToken observed_fragment;
    std::array<FragmentToken, fragment_trace_capacity> observed_contexts{}, cancelled_contexts{};
    unsigned observed_count = 0;
    std::uint64_t next_fragment_id = 1, primitive_id = 0;

    FragmentStage current_stage() const {
        switch (state) {
        case State::Idle: return FragmentStage::Idle;
        case State::EdgeLaunch: case State::EdgeWait: case State::EdgeCapture:
        case State::OffsetY: case State::OffsetX: return FragmentStage::Setup;
        case State::Raster: return FragmentStage::Raster;
        case State::Drain: return FragmentStage::Drain;
        }
        throw std::logic_error("unknown tile controller state");
    }
    void start_event(FragmentStage stage, bool enabled = true) {
        event = {};
        event.stage = stage;
        event.enabled = enabled;
        event.inflight_before = observed_count;
    }
    void finish_event() {
        event.cycle = counters.cycles;
        event.inflight_after = observed_count;
        if (observer) observer(event);
    }
    void accept_fragment(int px, int py, int slot = -1) {
        auto& token = slot < 0 ? observed_fragment : observed_contexts.at(static_cast<unsigned>(slot));
        if (token.id != 0 || event.accepted)
            throw std::logic_error("fragment observer detected an overwritten context");
        if (next_fragment_id == 0)
            throw std::overflow_error("fragment observer ID space exhausted");
        token = {next_fragment_id++, primitive_id, px, py, local_address(px, py), need_texture};
        ++observed_count;
        event.accepted = true;
        event.accepted_fragment = token;
    }
    void retire_fragment(FragmentRetirement reason, bool color = false, bool depth = false, int slot = -1) {
        auto& token = slot < 0 ? observed_fragment : observed_contexts.at(static_cast<unsigned>(slot));
        if (token.id == 0 || event.retired)
            throw std::logic_error("fragment observer detected retirement without acceptance");
        event.retired = true;
        event.retired_fragment = token;
        event.retirement = reason;
        event.color_write = color;
        event.depth_write = depth;
        token = {}; --observed_count;
    }

    void require_idle() const {
        if (state != State::Idle || pending.valid || fragments.busy())
            throw std::logic_error("hardware tile command requires a drained pipeline");
    }
    void banks(const BsramCycle& color, const BsramCycle& low, const BsramCycle& high) {
        color_bank().tick(color); low_bank().tick(low); high_bank().tick(high);
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
    p.fragments.reset();
    p.start_event(FragmentStage::Reset);
    if (p.observed_fragment.id != 0) p.retire_fragment(FragmentRetirement::Cancelled);
    if (p.observed_count != 0) {
        if (p.event.retired) throw std::logic_error("simultaneous fast and streamed fragment reset");
        unsigned count = 0;
        for (auto& token : p.observed_contexts) {
            if (token.id != 0) { p.cancelled_contexts[count++] = token; token = {}; }
        }
        if (count != p.observed_count) throw std::logic_error("fragment cancellation lost a context");
        p.event.cancelled_count = count; p.event.cancelled_fragments = p.cancelled_contexts.data();
        p.observed_count = 0;
    }
    p.state = Impl::State::Idle; p.pending = {}; p.counters = {};
    p.transfer_read_valid = false;
    const BsramCycle reset_output{false, 0, 0, false, 0, true};
    p.banks(reset_output, reset_output, reset_output);
    p.multiplier.tick({0, 0, false, true});
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
            result.color[beat - 1] = p.color_bank().output();
            result.depth[beat - 1] = p.low_bank().output() |
                (static_cast<std::uint32_t>(p.high_bank().output()) << 16);
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
    return {p.color_bank().output(), p.low_bank().output() |
            (static_cast<std::uint32_t>(p.high_bank().output()) << 16)};
}
void OpaqueTile::exchange_attachment_banks(AttachmentBanks*& idle_banks) {
    auto& p = *impl_; p.require_idle();
    if (!idle_banks || idle_banks == p.active_banks)
        throw std::invalid_argument("attachment role exchange requires a distinct physical bank set");
    if (!p.connected_banks) p.connected_banks = idle_banks;
    const auto expected = p.active_banks == &p.owned_banks ? p.connected_banks : &p.owned_banks;
    if (idle_banks != expected)
        throw std::invalid_argument("attachment role mux only connects two fixed bank sets");
    p.start_event(FragmentStage::Transfer);
    // Pointer exchange is the one-bit hardware role-select update. Neither
    // fixed array is assigned, copied, read or written on this control edge.
    auto* previous = p.active_banks;
    p.active_banks = idle_banks; idle_banks = previous;
    p.transfer_read_valid = false;
    p.multiplier.tick({0, 0, false, false});
    ++p.counters.cycles;
    p.finish_event();
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
    p.origin = tile_origin; p.pending = {};
    p.transfer_read_valid = false;
    p.triangle_draw = triangle_draw;
    p.sprite_draw = sprite_draw;
    p.draw_state = state;
    p.need_texture = need_texture;
    if (need_texture) p.fragments.configure(state);
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
void OpaqueTile::drain_external_memory_edge() {
    auto& p = *impl_;
    p.start_event(FragmentStage::Transfer);
    p.fragments.drain_external();
    p.banks({}, {}, {}); p.multiplier.tick({0, 0, false, false});
    ++p.counters.cycles; p.finish_event();
}
const TileStatistics& OpaqueTile::statistics() const { return impl_->counters; }
const FragmentCycle& OpaqueTile::last_cycle() const { return impl_->event; }
void OpaqueTile::set_cycle_observer(FragmentCycleObserver observer) { impl_->observer = std::move(observer); }

void OpaqueTile::tick(bool enable) {
    auto& p = *impl_;
    if (!enable) {
        if (p.need_texture) (void)p.fragments.tick({}, 0, false);
        ++p.counters.stalls; p.quiet_edge(p.current_stage(), false); return;
    }
    p.start_event(p.current_stage());
    ++p.counters.cycles;
    BsramCycle color_port, low_port, high_port;
    Mult18Cycle multiply_port{0, 0, false, false};
    bool depth_write = false;
    std::uint16_t write_address = 0;
    std::uint32_t write_depth = 0;
    if (p.need_texture && (p.state == Impl::State::Raster || p.state == Impl::State::Drain)) {
        TexturedFragmentPipeline::Input input;
        int px = p.x, py = p.y;
        bool inside = false;
        if (p.state == Impl::State::Raster) {
            Sample2D point;
            inside = true;
            if (p.triangle_draw) {
                for (std::size_t i = 0; i < 3; ++i)
                    inside = inside && (p.edge_current[i] > 0 ||
                        (p.edge_current[i] == 0 && p.triangle.edges[i].inclusive));
                input.q = p.plane_current[4]; input.u = p.plane_current[5]; input.v = p.plane_current[6];
            } else {
                point = p.walker.sample(); px = point.x; py = point.y; inside = point.covered;
                input.source_x = point.source_x; input.source_y = point.source_y;
            }
            input.valid = inside; input.sprite = p.sprite_draw;
            input.address = p.local_address(px, py); input.depth = p.sample_depth(); input.factor = p.sample_factor();
        }
        const bool perspective_active = p.fragments.perspective_busy();
        const bool texture_active = p.texture.inflight() != 0;
        const auto stored = p.draw_state.depth_test ? p.low_bank().output() |
            (static_cast<std::uint32_t>(p.high_bank().output()) << 16) : 0;
        const auto result = p.fragments.tick(input, stored);
        if (perspective_active || p.fragments.perspective_busy()) ++p.counters.perspective_cycles;
        if (texture_active || p.texture.inflight() != 0) ++p.counters.texture_cycles;
        p.sync_texture_statistics();
        if (result.retired) {
            p.retire_fragment(result.reason, result.color_write, result.depth_write, result.retired_slot);
            if (result.reason == FragmentRetirement::DepthRejected) ++p.counters.depth_rejected;
            else {
                ++p.counters.texture_samples;
                if (result.reason == FragmentRetirement::KeyRejected) ++p.counters.keyed_rejected;
            }
            if (result.color_write) {
                color_port = {true, result.address, result.color, false, 0, false}; ++p.counters.color_writes;
            }
            if (result.depth_write) {
                low_port = {true, result.address, static_cast<std::uint16_t>(result.depth), false, 0, false};
                high_port = {true, result.address, static_cast<std::uint16_t>(result.depth >> 16), false, 0, false};
                ++p.counters.depth_writes;
            }
        }
        if (p.state == Impl::State::Raster && (!inside || result.accepted)) {
            p.event.candidate = true;
            if (p.triangle_draw || inside) ++p.counters.candidates;
            if (inside) {
                p.event.covered = true;
                p.accept_fragment(px, py, result.accepted_slot); ++p.counters.covered_fragments;
                if (p.draw_state.depth_test) {
                    // Every walker visits an address at most once per
                    // primitive, and primitives drain before the next begins.
                    if (result.depth_write && result.address == input.address)
                        throw std::logic_error("textured walker repeated a live depth address");
                    low_port.read_enable = high_port.read_enable = true;
                    low_port.read_address = high_port.read_address = input.address;
                }
            }
            p.advance_candidate();
        }
        if (p.state == Impl::State::Drain && !p.fragments.busy()) p.state = Impl::State::Idle;
        p.banks(color_port, low_port, high_port);
        p.multiplier.tick(multiply_port);
        p.finish_event();
        return;
    }
    if (p.pending.valid) {
        const auto& f = p.pending;
        const auto previous = f.depth_test ?
            (f.forwarded ? f.forwarded_depth : p.low_bank().output() |
             (static_cast<std::uint32_t>(p.high_bank().output()) << 16)) : 0;
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
        (pass.depth_load != LoadOp::Load && pass.depth_load != LoadOp::Clear) ||
        (pass.depth_store != StoreOp::Store && pass.depth_store != StoreOp::Discard))
        throw std::invalid_argument("unsupported hardware pass load/store operation");
    require_depth(pass.clear_depth);
    if (pass.depth_load == LoadOp::Load) for (const auto value : depth) require_depth(value);
    for (const auto& primitive : pass.primitives) validate_record(primitive);
    const auto bins = bin_primitives(pass.primitives);
    statistics = {};
    for (const auto& bin : bins) statistics.tile_references += bin.size();
    OpaqueTile slice;
    for (int tile = 0; tile < tile_count; ++tile) {
        // A SKIP pass leaves tiles without primitives untouched.
        if (pass.skip_empty_tiles && bins[static_cast<std::size_t>(tile)].empty()) continue;
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
            color[global] = output.color[local];
            if (pass.depth_store == StoreOp::Store) depth[global] = output.depth[local];
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
