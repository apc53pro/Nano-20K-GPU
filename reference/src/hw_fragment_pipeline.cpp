#include "nano_gpu/hw/fragment_pipeline.hpp"

#include "nano_gpu/hw/bsram.hpp"
#include "nano_gpu/hw/mult18x18.hpp"
#include "nano_gpu/hw/reciprocal.hpp"
#include "nano_gpu/hw/texture.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace nano_gpu::hw {
namespace {

bool depth_accept(DepthFunc function, std::uint32_t incoming, std::uint32_t stored) {
    switch (function) {
    case DepthFunc::Less: return incoming < stored;
    case DepthFunc::LessEqual: return incoming <= stored;
    case DepthFunc::Always: return true;
    }
    throw std::invalid_argument("invalid textured-pipeline depth function");
}

int addressed_coordinate(std::int32_t q16, int size, AddressMode mode) {
    const auto integer = q16 >= 0 ? q16 >> 16 :
        -static_cast<int>((-static_cast<std::int64_t>(q16) + 65535) >> 16);
    if (mode == AddressMode::Clamp) return std::clamp(integer, 0, size - 1);
    if (mode == AddressMode::Repeat)
        return static_cast<int>(static_cast<std::uint32_t>(integer) & static_cast<unsigned>(size - 1));
    throw std::invalid_argument("invalid textured-pipeline address mode");
}

Color expanded(std::uint16_t texel) {
    const auto r = static_cast<std::uint8_t>((texel >> 11) & 31u);
    const auto g = static_cast<std::uint8_t>((texel >> 5) & 63u);
    const auto b = static_cast<std::uint8_t>(texel & 31u);
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)),
            static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2))};
}

std::uint16_t packed(Color color) {
    return static_cast<std::uint16_t>(((color.r >> 3) << 11) | ((color.g >> 2) << 5) | (color.b >> 3));
}

std::uint8_t modulated_channel(const Mult18x18& multiplier) {
    if (!multiplier.output_known()) throw std::logic_error("shader DSP product was not registered");
    const auto product = static_cast<std::uint32_t>(multiplier.product()) + 128u;
    return static_cast<std::uint8_t>((product + (product >> 8)) >> 8);
}

} // namespace

struct TexturedFragmentPipeline::Impl {
    enum class Completion : std::uint8_t { Commit, DepthRejected, KeyRejected };
    struct DepthStage {
        bool valid = false, sprite = false, forwarded = false;
        std::uint8_t slot = 0;
        std::uint32_t depth = 0, forwarded_depth = 0;
        std::int64_t u = 0, v = 0, q = std::int64_t{1} << 29;
        int x = 0, y = 0;
    };
    struct CoordinateRead { bool valid = false; std::uint8_t slot = 0; int x = 0, y = 0; };
    struct Coordinates {
        bool valid = false;
        std::uint8_t slot = 0;
        int x = 0, y = 0;
        Color factor{};
    };
    struct TextureContext { std::uint8_t slot = 0; Color factor{}; };
    struct ShadeStage { bool valid = false; std::uint8_t slot = 0; };
    struct HeadRead { bool valid = false; std::uint8_t slot = 0; Completion completion = Completion::Commit; };

    TextureUnit& texture;
    PerspectivePipeline perspective;
    std::array<Sdp1kx16, context_bsram_blocks> context;
    std::array<Mult18x18, shade_mult18_equivalents> shade;
    // The only per-context FFs are completion flags. Address/depth, factors,
    // and RGB565 output have no parallel host-array execution path.
    std::array<bool, capacity> ready{};
    std::array<Completion, capacity> completion{};
    unsigned head = 0, tail = 0, count = 0;
    HeadRead head_read;
    DepthStage depth;
    CoordinateRead coordinate_read;
    Coordinates coordinates;
    // Eight fixed register entries correspond one-for-one to the texture
    // pipeline's bounded accepted requests; the factor is24 bits plus6-bit tag.
    std::array<TextureContext, TextureUnit::streaming_capacity> texture_context{};
    unsigned texture_head = 0, texture_tail = 0, texture_count = 0;
    std::array<ShadeStage, Mult18x18::enabled_edge_latency> shade_meta{};
    RenderState state;
    bool configured = false;

    explicit Impl(TextureUnit& unit) : texture(unit) {}

    bool busy() const {
        return count != 0 || depth.valid || coordinate_read.valid || coordinates.valid || head_read.valid ||
            texture_count != 0 || perspective.busy() || perspective.output().valid || texture.busy() ||
            std::any_of(shade_meta.begin(), shade_meta.end(), [](const ShadeStage& stage) { return stage.valid; });
    }
    void tick_context(const std::array<BsramCycle, context_bsram_blocks>& ports) {
        for (unsigned bank = 0; bank < context.size(); ++bank) context[bank].tick(ports[bank]);
    }
    void freeze_units() {
        perspective.tick({}, false);
        for (auto& multiplier : shade) multiplier.tick({0, 0, false, false});
        tick_context({});
    }
    void complete(unsigned slot, Completion reason) {
        if (ready[slot]) throw std::logic_error("fragment context completed twice");
        ready[slot] = true; completion[slot] = reason;
    }
};

TexturedFragmentPipeline::TexturedFragmentPipeline(TextureUnit& texture)
    : impl_(std::make_unique<Impl>(texture)) {
    texture.pipeline_mode(true);
    reset();
}
TexturedFragmentPipeline::~TexturedFragmentPipeline() = default;
TexturedFragmentPipeline::TexturedFragmentPipeline(TexturedFragmentPipeline&&) noexcept = default;
TexturedFragmentPipeline& TexturedFragmentPipeline::operator=(TexturedFragmentPipeline&&) noexcept = default;

void TexturedFragmentPipeline::configure(const RenderState& state) {
    auto& p = *impl_;
    if (p.busy()) throw std::logic_error("configure requires a drained textured fragment pipeline");
    if (!state.texture) throw std::invalid_argument("textured fragment pipeline requires texture metadata");
    p.texture.pipeline_mode(true);
    p.state = state; p.configured = true;
}

TexturedFragmentPipeline::Result TexturedFragmentPipeline::tick(const Input& input,
                                                               std::uint32_t stored_depth, bool enable) {
    auto& p = *impl_;
    Result result;
    if (!enable) { p.freeze_units(); p.texture.tick(false); return result; }
    if (input.valid && !p.configured)
        throw std::logic_error("textured fragment input before configuration");
    std::array<BsramCycle, context_bsram_blocks> ports{};
    std::array<Mult18Cycle, shade_mult18_equivalents> shade_ports;
    for (auto& port : shade_ports) port = {0, 0, true, false};

    // Capture OLD native RAM outputs. An OLD ready flag may launch the next
    // head read on this edge, permitting consecutive ordered retirements.
    if (p.head_read.valid) {
        const auto finished = p.head_read;
        if (finished.slot != p.head || p.count == 0 || !p.ready[finished.slot])
            throw std::logic_error("fragment ROB head read lost its owned context");
        result.retired = true; result.retired_slot = finished.slot;
        result.address = p.context[0].output();
        result.depth = p.context[1].output() | (std::uint32_t{p.context[2].output()} << 16);
        switch (finished.completion) {
        case Impl::Completion::Commit:
            result.reason = FragmentRetirement::Commit;
            result.color = p.context[5].output();
            result.color_write = p.state.color_write; result.depth_write = p.state.depth_write;
            break;
        case Impl::Completion::DepthRejected: result.reason = FragmentRetirement::DepthRejected; break;
        case Impl::Completion::KeyRejected: result.reason = FragmentRetirement::KeyRejected; break;
        }
        p.ready[finished.slot] = false;
        p.head = (p.head + 1) & (capacity - 1); --p.count;
        p.head_read = {};
    }
    if (p.count != 0 && p.ready[p.head]) {
        p.head_read = {true, static_cast<std::uint8_t>(p.head), p.completion[p.head]};
        for (const unsigned bank : {0u, 1u, 2u, 5u}) {
            ports[bank].read_enable = true;
            ports[bank].read_address = static_cast<std::uint16_t>(p.head);
        }
    }

    // Shader output is not visible to the ROB read scheduler above until the
    // next edge. Three distinct registered18x18 products operate in parallel.
    if (p.shade_meta.back().valid) {
        const auto slot = p.shade_meta.back().slot;
        const Color color{modulated_channel(p.shade[0]), modulated_channel(p.shade[1]),
                          modulated_channel(p.shade[2])};
        ports[5].write_enable = true; ports[5].write_address = slot; ports[5].write_data = packed(color);
        p.complete(slot, Impl::Completion::Commit);
    }
    for (unsigned stage = static_cast<unsigned>(p.shade_meta.size()) - 1; stage != 0; --stage)
        p.shade_meta[stage] = p.shade_meta[stage - 1];
    p.shade_meta[0] = {};

    // Consume only an OLD texture output and its oldest register-FIFO context.
    // Key rejection uses the raw565 texel before shading and either attachment.
    if (p.texture.output_valid()) {
        if (p.texture_count == 0) throw std::logic_error("texture output has no fragment context");
        const auto retained = p.texture_context[p.texture_head];
        const auto texel = p.texture.output();
        if (!p.texture.pop()) throw std::logic_error("texture output could not reserve its retirement edge");
        p.texture_head = (p.texture_head + 1) & (TextureUnit::streaming_capacity - 1); --p.texture_count;
        if (p.state.color_key && texel == *p.state.color_key) p.complete(retained.slot, Impl::Completion::KeyRejected);
        else {
            const auto color = expanded(texel);
            shade_ports = {{{color.r, retained.factor.r, true, false},
                            {color.g, retained.factor.g, true, false},
                            {color.b, retained.factor.b, true, false}}};
            p.shade_meta[0] = {true, retained.slot};
        }
    }

    // Two elastic coordinate stages hide the synchronous factor-bank read.
    // Consume OLD ready coordinates before capturing the OLD B-port output;
    // a newly captured coordinate never enters the texture unit on this edge.
    if (p.coordinates.valid && p.texture_count < TextureUnit::streaming_capacity &&
        p.texture.inflight() < TextureUnit::streaming_capacity && p.texture.input_ready()) {
        if (!p.texture.push(p.coordinates.x, p.coordinates.y))
            throw std::logic_error("texture input readiness changed before its edge");
        p.texture_context[p.texture_tail] = {p.coordinates.slot, p.coordinates.factor};
        p.texture_tail = (p.texture_tail + 1) & (TextureUnit::streaming_capacity - 1); ++p.texture_count;
        p.coordinates = {};
    }
    if (p.coordinate_read.valid && !p.coordinates.valid) {
        const auto factor_low = p.context[3].output(), factor_high = p.context[4].output();
        p.coordinates = {true, p.coordinate_read.slot, p.coordinate_read.x, p.coordinate_read.y,
                         {static_cast<std::uint8_t>(factor_low), static_cast<std::uint8_t>(factor_low >> 8),
                          static_cast<std::uint8_t>(factor_high)}};
        p.coordinate_read = {};
    }
    const bool perspective_enable = !p.coordinate_read.valid;
    bool factor_read_used = false;
    const auto& uv = p.perspective.output();
    if (perspective_enable && uv.valid) {
        if (uv.invalid_q) throw std::out_of_range("reconstructed texture q exceeds its numeric domain");
        if (uv.overflow) throw std::overflow_error("reconstructed UV exceeds signed Q16");
        p.coordinate_read = {true, uv.tag,
            addressed_coordinate(uv.u, p.state.texture->width(), p.state.address_u),
            addressed_coordinate(uv.v, p.state.texture->height(), p.state.address_v)};
        factor_read_used = true;
    }

    PerspectivePipeline::Input perspective_input;
    // The depth register's B-port response was established by the enclosing
    // tile on the previous edge. A blocked result retains that same register.
    if (p.depth.valid) {
        const bool passing = !p.state.depth_test ||
            depth_accept(p.state.depth_func, p.depth.depth,
                         p.depth.forwarded ? p.depth.forwarded_depth : stored_depth);
        if (!passing) { p.complete(p.depth.slot, Impl::Completion::DepthRejected); p.depth = {}; }
        else if (p.depth.sprite) {
            if (!p.coordinate_read.valid && !factor_read_used) {
                p.coordinate_read = {true, p.depth.slot, p.depth.x, p.depth.y};
                factor_read_used = true; p.depth = {};
            }
        } else if (perspective_enable) {
            perspective_input = {p.depth.u, p.depth.v, p.depth.q, p.depth.slot, true};
            p.depth = {};
        }
    }
    if (factor_read_used) {
        for (const unsigned bank : {3u, 4u}) {
            ports[bank].read_enable = true; ports[bank].read_address = p.coordinate_read.slot;
        }
    }

    // A retiring OLD head frees its slot before this new input is captured.
    // The next-head read above necessarily names a different owned context;
    // native same-address A/B collisions therefore remain forbidden.
    if (input.valid && !p.depth.valid && p.count < capacity) {
        if (input.address >= tile_size * tile_size || input.depth > depth_max || input.forwarded_depth > depth_max)
            throw std::out_of_range("textured fragment exceeds tile address or D24 width");
        const auto slot = static_cast<std::uint8_t>(p.tail);
        result.accepted = true; result.accepted_slot = slot;
        const bool modulation = !input.sprite && p.state.color_write &&
            (p.state.shade == ShadeMode::TextureFlatModulate || p.state.shade == ShadeMode::TextureGouraudModulate);
        const auto factor = modulation ? input.factor : Color{255, 255, 255};
        const std::array<std::uint16_t, 5> words{{input.address, static_cast<std::uint16_t>(input.depth),
            static_cast<std::uint16_t>(input.depth >> 16),
            static_cast<std::uint16_t>(factor.r | (std::uint16_t{factor.g} << 8)), factor.b}};
        for (unsigned bank = 0; bank < words.size(); ++bank) {
            ports[bank].write_enable = true; ports[bank].write_address = slot; ports[bank].write_data = words[bank];
        }
        if (p.ready[slot]) throw std::logic_error("fragment input overwrote a ready ROB slot");
        p.depth = {true, input.sprite, input.forwarded, slot, input.depth, input.forwarded_depth,
                   input.u, input.v, input.q, input.source_x, input.source_y};
        p.tail = (p.tail + 1) & (capacity - 1); ++p.count;
    }

    p.perspective.tick(perspective_input, perspective_enable);
    for (unsigned channel = 0; channel < p.shade.size(); ++channel) p.shade[channel].tick(shade_ports[channel]);
    p.tick_context(ports);
    p.texture.tick();
    return result;
}

bool TexturedFragmentPipeline::input_ready() const { return !impl_->depth.valid && impl_->count < capacity; }
bool TexturedFragmentPipeline::busy() const { return impl_->busy(); }
unsigned TexturedFragmentPipeline::inflight() const { return impl_->count; }
bool TexturedFragmentPipeline::perspective_busy() const { return impl_->perspective.busy(); }
bool TexturedFragmentPipeline::external_memory_pending() const { return impl_->texture.external_memory_pending(); }

void TexturedFragmentPipeline::drain_external() {
    impl_->freeze_units();
    impl_->texture.tick();
}

void TexturedFragmentPipeline::reset() {
    auto& p = *impl_;
    // Diagnose before changing any owned physical context or unit state.
    p.texture.reset();
    p.perspective.reset();
    p.ready = {}; p.completion = {};
    p.head = p.tail = p.count = 0; p.head_read = {}; p.depth = {};
    p.coordinate_read = {}; p.coordinates = {};
    p.texture_context = {}; p.texture_head = p.texture_tail = p.texture_count = 0;
    p.shade_meta = {}; p.configured = false; p.state = {};
    for (auto& multiplier : p.shade) multiplier.tick({0, 0, false, true});
    for (auto& bank : p.context) bank.tick({false, 0, 0, false, 0, true});
}

} // namespace nano_gpu::hw
