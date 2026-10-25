#include "nano_gpu/hw/texture.hpp"

#include "nano_gpu/hw/bsram.hpp"
#include "nano_gpu/hw/mult18x18.hpp"
#include "nano_gpu/hw/sdram.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace nano_gpu::hw {
namespace {

// The Image constructor enforces the same maximum allocation extent. These
// bounds make the modeled word address 22 bits and each cache tag 12 bits.
constexpr std::uint32_t maximum_texture_words = (0x800000u - 0x258000u) / 2;

void require_texture(const std::shared_ptr<const Image>& texture) {
    if (!texture || !texture->texture_compatible()) {
        throw std::invalid_argument("hardware texture unit requires a power-of-two RGB565 image");
    }
}

void require_descriptor(const TextureDescriptor& descriptor) {
    const auto power_of_two = [](std::uint32_t value) {
        return value != 0 && (value & (value - 1u)) == 0;
    };
    const auto extent = static_cast<std::uint64_t>(descriptor.pitch_bytes) * descriptor.height;
    if (descriptor.base < heap_base || (descriptor.base & 3u) != 0 ||
        descriptor.width > 1024 || descriptor.height > 1024 ||
        !power_of_two(descriptor.width) || !power_of_two(descriptor.height) ||
        (descriptor.pitch_bytes & 3u) != 0 ||
        descriptor.pitch_bytes < descriptor.width * 2u ||
        extent > memory_bytes || static_cast<std::uint64_t>(descriptor.base) + extent > memory_bytes) {
        throw std::invalid_argument("hardware texture descriptor exceeds the aligned SDRAM heap");
    }
}

} // namespace

struct TextureUnit::Impl {
    enum class State {
        Idle, Address, Tag, ResponseWait, ExternalRequest, ExternalWait,
        ExternalRead, ExternalCapture, Refill, CacheRead, Capture
    };

    Sdp1kx16 bank;
    std::array<std::uint32_t, line_count> tags{};
    std::array<bool, line_count> valid{};
    std::shared_ptr<const Image> texture;
    TextureDescriptor descriptor;
    SdramController* external_memory = nullptr;
    bool external_request_pending = false;
    unsigned external_words = 0;
    // One registered word bridges the controller slot read port and the
    // 16-bit cache write port. No parallel cache-line response copy exists.
    std::uint32_t external_word = 0;
    TextureStatistics counters;
    State state = State::Idle;
    bool result_valid = false;
    bool scratch_read_valid = false;
    std::uint16_t result = 0;
    std::uint32_t address = 0;
    std::uint32_t shifted_stride = 0;
    std::uint32_t y_bits = 0;
    unsigned address_bit = 0;
    std::uint32_t line_base = 0;
    std::uint32_t tag = 0;
    unsigned slot = 0;
    unsigned word_in_line = 0;
    unsigned response_wait = 0;
    unsigned refill_word = 0;

    // Static alternative to the serial oracle above. Two physical MULT18X18
    // instances evaluate Y * (pitch_low17 + pitch_high * 2^17). Both operands
    // fit the positive range of signed18; no host-wide multiply supplies the
    // address result. Metadata follows the three registered DSP stages.
    bool streaming = false;
    Mult18x18 pitch_low, pitch_high;
    struct Input { bool valid = false; unsigned x = 0, y = 0; } input;
    struct MultiplyMeta { bool valid = false; unsigned x = 0; };
    std::array<MultiplyMeta, 3> multiply_meta{};
    bool linear_valid = false;
    std::uint32_t linear_address = 0;
    bool lookup_valid = false;
    unsigned lookup_address = 0;
    bool cache_read_pending = false;
    bool output_valid = false, output_pop = false;
    std::uint16_t output = 0;
    bool replay_miss = false;

    unsigned inflight() const {
        unsigned count = input.valid + linear_valid + lookup_valid +
            cache_read_pending + output_valid;
        for (const auto& stage : multiply_meta) count += stage.valid;
        return count;
    }

    void require_idle() const {
        if (state != State::Idle || (streaming && inflight() != 0))
            throw std::logic_error("hardware texture unit already has an outstanding request");
    }

    void control_edge() {
        bank.tick({});
        ++counters.cycles;
    }

    // Functional backing-store adapter only. The timed cache controller emits
    // a linear word address; this host code supplies a corresponding response.
    // Division/modulo here do not stand for address-generation hardware. Row
    // padding and out-of-allocation words in a final cache line return zero.
    std::uint16_t backing_word(std::uint32_t word) const {
        const auto pitch_words = static_cast<std::uint32_t>(texture->pitch_bytes() / 2);
        const auto row = word / pitch_words;
        const auto column = word % pitch_words;
        if (row >= static_cast<std::uint32_t>(texture->height()) ||
            column >= static_cast<std::uint32_t>(texture->width())) return 0;
        return texture->at(static_cast<int>(column), static_cast<int>(row));
    }

    bool service_refill(BsramCycle& memory, bool pipeline);
    void pipeline_tick();
};

bool TextureUnit::Impl::service_refill(BsramCycle& memory, bool pipeline) {
    switch (state) {
    case State::ResponseWait:
        if (--response_wait == 0) state = State::Refill;
        break;
    case State::ExternalRequest: {
        // The allocation is four-byte aligned, and each cache line holds an
        // integral number of 32-bit words. A final short line is never read
        // past that allocation; only its unused cache tail is padded with zero.
        const auto allocation_words = static_cast<std::uint64_t>(descriptor.pitch_bytes) *
                                      descriptor.height / 4u;
        const auto first_word = line_base >> 1;
        if (first_word >= allocation_words)
            throw std::logic_error("hardware texture refill starts outside its allocation");
        MemoryRequest request;
        request.address = descriptor.base + (line_base << 1);
        request.words = static_cast<unsigned>(std::min<std::uint64_t>(
            line_words / 2, allocation_words - first_word));
        request.tag = line_base;
        if (external_memory->submit_prepared(MemoryClient::Texture, request)) {
            external_request_pending = true;
            external_words = request.words;
            state = State::ExternalWait;
        } else ++counters.memory_wait_cycles;
        break;
    }
    case State::ExternalWait:
        if (external_memory->response_ready(MemoryClient::Texture)) {
            const auto response = external_memory->response_info(MemoryClient::Texture);
            if (response.tag != line_base || response.words != external_words ||
                response.words == 0 || response.words > line_words / 2)
                throw std::logic_error("hardware texture SDRAM response does not match its request");
            state = State::ExternalRead;
        } else ++counters.memory_wait_cycles;
        break;
    case State::ExternalRead:
        if (external_memory->request_response_word(MemoryClient::Texture, refill_word >> 1))
            state = State::ExternalCapture;
        else ++counters.memory_wait_cycles;
        break;
    case State::ExternalCapture:
        if (!external_memory->response_word_valid(MemoryClient::Texture) ||
            external_memory->response_word_offset(MemoryClient::Texture) != (refill_word >> 1))
            throw std::logic_error("hardware texture slot read did not cross its synchronous edge");
        external_word = external_memory->response_word(MemoryClient::Texture);
        // The registered word is first consumed by the cache on a later edge.
        state = State::Refill;
        break;
    case State::Refill:
        memory.write_enable = true;
        memory.write_address = static_cast<std::uint16_t>((slot << 4) | refill_word);
        memory.write_data = external_memory ? static_cast<std::uint16_t>(
            external_word >> ((refill_word & 1u) << 4)) :
            backing_word(line_base + refill_word);
        ++counters.refill_words;
        ++refill_word;
        if (external_memory && external_request_pending && refill_word == external_words * 2) {
            external_memory->release_response(MemoryClient::Texture);
            external_request_pending = false;
            external_word = 0; // Final short-line padding uses no SDRAM port.
        }
        if (refill_word == line_words) {
            tags[slot] = tag;
            valid[slot] = true;
            state = pipeline ? State::Idle : State::CacheRead;
        } else if (external_memory && external_request_pending && (refill_word & 1u) == 0)
            state = State::ExternalRead;
        break;
    default: return false;
    }
    return true;
}

void TextureUnit::Impl::pipeline_tick() {
    BsramCycle memory;
    bool advance = !output_valid || output_pop;
    if (output_pop) {
        output_valid = false; output_pop = false;
        ++counters.pipeline_outputs;
    }
    // The cache B output belongs to a read established on an earlier edge.
    // It may remain pending behind a full result register while the A port
    // refills another line; writes do not change that registered B output.
    if (advance && cache_read_pending) {
        output = bank.output(); output_valid = true;
        cache_read_pending = false;
    }

    const bool servicing = state != State::Idle;
    if (servicing) {
        if (!service_refill(memory, true))
            throw std::logic_error("streaming texture entered a serial-only state");
        advance = false;
    } else if (advance) {
        // An older lookup can issue its read even when this edge discovers a
        // younger miss. Refilling begins on a later edge, so no cache read
        // collides with the first refill write and completion order is kept.
        if (lookup_valid) {
            memory.read_enable = true;
            memory.read_address = static_cast<std::uint16_t>(lookup_address);
            cache_read_pending = true; lookup_valid = false;
        }
        if (linear_valid) {
            const auto next_slot = (linear_address >> 4) & (line_count - 1u);
            const auto next_tag = linear_address >> 10;
            if (valid[next_slot] && tags[next_slot] == next_tag) {
                if (!replay_miss) ++counters.hits;
                replay_miss = false;
                lookup_address = (next_slot << 4) | (linear_address & (line_words - 1u));
                lookup_valid = true;
            } else {
                if (replay_miss) throw std::logic_error("refilled texture line was lost before replay");
                ++counters.misses;
                line_base = linear_address & ~(line_words - 1u);
                slot = next_slot; tag = next_tag;
                valid[slot] = false; refill_word = 0; replay_miss = true;
                if (external_memory) state = State::ExternalRequest;
                else { response_wait = backing_response_edges; state = State::ResponseWait; }
                advance = false;
            }
        }
    }

    Mult18Cycle low{0, 0, false, false}, high{0, 0, false, false};
    if (advance) {
        // A DSP product becomes an OLD registered output before its consumer
        // captures it here. The matching metadata spends three enabled edges
        // in the same register chain as the primitive's A/B, PIPE and OUT.
        linear_valid = multiply_meta.back().valid;
        if (linear_valid) {
            if (!pitch_low.output_known() || !pitch_high.output_known())
                throw std::logic_error("texture address product was not registered");
            const auto value = static_cast<std::uint64_t>(pitch_low.product()) +
                (static_cast<std::uint64_t>(pitch_high.product()) << 17) + multiply_meta.back().x;
            if (value >= maximum_texture_words)
                throw std::overflow_error("streaming texture address exceeds the bounded image heap");
            linear_address = static_cast<std::uint32_t>(value);
        }
        for (unsigned stage = static_cast<unsigned>(multiply_meta.size()) - 1; stage != 0; --stage)
            multiply_meta[stage] = multiply_meta[stage - 1];
        multiply_meta[0] = {input.valid, input.x};
        const auto pitch = external_memory ? descriptor.pitch_bytes / 2u :
            (texture ? static_cast<std::uint32_t>(texture->pitch_bytes() / 2) : 0u);
        low = {static_cast<std::int32_t>(input.valid ? input.y : 0),
               static_cast<std::int32_t>(pitch & 0x1ffffu), true, false};
        high = {static_cast<std::int32_t>(input.valid ? input.y : 0),
                static_cast<std::int32_t>(pitch >> 17), true, false};
        if (input.valid) ++counters.pipeline_inputs;
        input = {};
    } else if (inflight() != 0) ++counters.pipeline_backpressure_cycles;
    pitch_low.tick(low); pitch_high.tick(high);
    bank.tick(memory);
}

TextureUnit::TextureUnit() : impl_(std::make_unique<Impl>()) { reset(); }
TextureUnit::~TextureUnit() = default;
TextureUnit::TextureUnit(TextureUnit&&) noexcept = default;
TextureUnit& TextureUnit::operator=(TextureUnit&&) noexcept = default;

void TextureUnit::bind(std::shared_ptr<const Image> texture) {
    auto& p = *impl_;
    p.require_idle();
    require_texture(texture);
    if (p.external_memory || p.texture.get() != texture.get()) p.valid.fill(false);
    p.texture = std::move(texture);
    p.external_memory = nullptr;
    p.descriptor = {};
    p.result_valid = false;
    p.scratch_read_valid = false;
    p.control_edge();
}

void TextureUnit::bind_external(const TextureDescriptor& descriptor, SdramController& memory) {
    auto& p = *impl_;
    p.require_idle();
    require_descriptor(descriptor);
    if (p.external_memory != &memory || p.descriptor != descriptor) p.valid.fill(false);
    p.texture.reset();
    p.descriptor = descriptor;
    p.external_memory = &memory;
    p.result_valid = false;
    p.scratch_read_valid = false;
    p.control_edge();
}

void TextureUnit::invalidate() {
    auto& p = *impl_;
    p.require_idle();
    p.valid.fill(false);
    p.result_valid = false;
    p.scratch_read_valid = false;
    p.control_edge();
}

void TextureUnit::scratch_write(std::uint16_t address, std::uint16_t value) {
    auto& p = *impl_; p.require_idle();
    if (address >= cache_words) throw std::out_of_range("copy cache word address exceeds ten bits");
    p.bank.tick({true, address, value, false, 0, false});
    p.valid.fill(false);
    p.result_valid = p.scratch_read_valid = false;
    ++p.counters.cycles;
}
void TextureUnit::scratch_read_request(std::uint16_t address) {
    auto& p = *impl_; p.require_idle();
    if (address >= cache_words) throw std::out_of_range("copy cache word address exceeds ten bits");
    p.bank.tick({false, 0, 0, true, address, false});
    p.result_valid = false;
    p.scratch_read_valid = true;
    ++p.counters.cycles;
}
std::uint16_t TextureUnit::scratch_read_result() const {
    const auto& p = *impl_; p.require_idle();
    if (!p.scratch_read_valid) throw std::logic_error("copy cache result has no preceding read request");
    return p.bank.output();
}

void TextureUnit::start(int x, int y) {
    auto& p = *impl_;
    if (p.streaming) throw std::logic_error("streaming texture requests use push()");
    p.require_idle();
    if (!p.external_memory) require_texture(p.texture);
    const auto width = p.external_memory ? p.descriptor.width : static_cast<unsigned>(p.texture->width());
    const auto height = p.external_memory ? p.descriptor.height : static_cast<unsigned>(p.texture->height());
    if (x < 0 || static_cast<unsigned>(x) >= width || y < 0 || static_cast<unsigned>(y) >= height) {
        throw std::out_of_range("hardware texture coordinate must be addressed before sampling");
    }
    p.address = static_cast<std::uint32_t>(x);
    p.shifted_stride = p.external_memory ? p.descriptor.pitch_bytes / 2u :
        static_cast<std::uint32_t>(p.texture->pitch_bytes() / 2);
    p.y_bits = static_cast<std::uint32_t>(y);
    p.address_bit = 0;
    p.result_valid = false;
    p.scratch_read_valid = false;
    p.state = Impl::State::Address;
    p.control_edge();
}

bool TextureUnit::busy() const {
    return impl_->state != Impl::State::Idle || (impl_->streaming && impl_->inflight() != 0);
}
bool TextureUnit::external_memory_pending() const { return impl_->external_request_pending; }
bool TextureUnit::result_valid() const { return impl_->result_valid; }

std::uint16_t TextureUnit::result() const {
    if (!impl_->result_valid) throw std::logic_error("hardware texture result is not valid");
    return impl_->result;
}

void TextureUnit::pipeline_mode(bool enabled) {
    auto& p = *impl_; p.require_idle();
    p.streaming = enabled; p.result_valid = false;
}
bool TextureUnit::pipeline_mode() const { return impl_->streaming; }
bool TextureUnit::input_ready() const {
    const auto& p = *impl_;
    return p.streaming && (p.external_memory || p.texture) && !p.input.valid;
}
bool TextureUnit::push(int x, int y) {
    auto& p = *impl_;
    if (!input_ready()) return false;
    const auto width = p.external_memory ? p.descriptor.width : static_cast<unsigned>(p.texture->width());
    const auto height = p.external_memory ? p.descriptor.height : static_cast<unsigned>(p.texture->height());
    if (x < 0 || static_cast<unsigned>(x) >= width || y < 0 || static_cast<unsigned>(y) >= height)
        throw std::out_of_range("streaming texture coordinate exceeds the bound image");
    p.input = {true, static_cast<unsigned>(x), static_cast<unsigned>(y)};
    if (p.inflight() > streaming_capacity)
        throw std::logic_error("streaming texture exceeded its bounded stage capacity");
    return true;
}
bool TextureUnit::output_valid() const { return impl_->streaming && impl_->output_valid; }
std::uint16_t TextureUnit::output() const {
    if (!output_valid()) throw std::logic_error("streaming texture output is not valid");
    return impl_->output;
}
bool TextureUnit::pop() {
    auto& p = *impl_;
    if (!p.streaming || !p.output_valid || p.output_pop) return false;
    p.output_pop = true; return true;
}
unsigned TextureUnit::inflight() const { return impl_->streaming ? impl_->inflight() : (busy() ? 1u : 0u); }

void TextureUnit::tick(bool enable) {
    auto& p = *impl_;
    ++p.counters.cycles;
    if (!enable) {
        p.bank.tick({});
        p.pitch_low.tick({0, 0, false, false}); p.pitch_high.tick({0, 0, false, false});
        ++p.counters.stalls;
        return;
    }
    if (p.streaming) { p.pipeline_tick(); return; }

    BsramCycle memory;
    switch (p.state) {
    case Impl::State::Idle:
        break;
    case Impl::State::Address:
        // 10-bit Y selects shifts of the bounded word stride. There is one
        // explicit 32-bit adder and one selected add per enabled edge.
        if ((p.y_bits & 1u) != 0) {
            if (p.address > maximum_texture_words ||
                p.shifted_stride > maximum_texture_words - p.address) {
                throw std::overflow_error("hardware texture address exceeds the bounded image heap");
            }
            p.address += p.shifted_stride;
        }
        p.y_bits >>= 1;
        if (++p.address_bit == address_edges) {
            p.line_base = p.address & ~(line_words - 1u);
            p.word_in_line = p.address & (line_words - 1u);
            p.slot = (p.address >> 4) & (line_count - 1u);
            p.tag = p.address >> 10;
            p.state = Impl::State::Tag;
        } else p.shifted_stride <<= 1;
        break;
    case Impl::State::Tag:
        if (p.valid[p.slot] && p.tags[p.slot] == p.tag) {
            ++p.counters.hits;
            p.state = Impl::State::CacheRead;
        } else {
            ++p.counters.misses;
            p.valid[p.slot] = false;
            p.refill_word = 0;
            if (p.external_memory) p.state = Impl::State::ExternalRequest;
            else {
                p.response_wait = backing_response_edges;
                p.state = Impl::State::ResponseWait;
            }
        }
        break;
    case Impl::State::ResponseWait: case Impl::State::ExternalRequest:
    case Impl::State::ExternalWait: case Impl::State::ExternalRead:
    case Impl::State::ExternalCapture: case Impl::State::Refill:
        (void)p.service_refill(memory, false);
        break;
    case Impl::State::CacheRead:
        memory.read_enable = true;
        memory.read_address = static_cast<std::uint16_t>((p.slot << 4) | p.word_in_line);
        p.state = Impl::State::Capture;
        break;
    case Impl::State::Capture:
        // Observe the register written by the prior rising edge. Capture
        // cannot use a response produced later in this same tick.
        p.result = p.bank.output();
        p.result_valid = true;
        p.state = Impl::State::Idle;
        break;
    }
    p.bank.tick(memory);
}

void TextureUnit::reset() {
    auto& p = *impl_;
    // Resetting a local consumer cannot retract an accepted memory request.
    // Its owner must first clock the consumer until that response is retired.
    if (p.external_request_pending)
        throw std::logic_error("drain the outstanding texture SDRAM response before reset");
    p.state = Impl::State::Idle;
    p.texture.reset();
    p.descriptor = {};
    p.external_memory = nullptr;
    p.external_words = 0;
    p.external_word = 0;
    p.valid.fill(false);
    p.tags.fill(0);
    p.result_valid = false;
    p.scratch_read_valid = false;
    p.result = 0;
    p.address = p.shifted_stride = p.y_bits = 0;
    p.address_bit = p.slot = p.word_in_line = p.response_wait = p.refill_word = 0;
    p.line_base = p.tag = 0;
    p.input = {}; p.multiply_meta = {};
    p.linear_valid = p.lookup_valid = p.cache_read_pending = false;
    p.output_valid = p.output_pop = p.replay_miss = false;
    p.output = 0;
    p.pitch_low.tick({0, 0, false, true}); p.pitch_high.tick({0, 0, false, true});
    p.counters = {};
    p.bank.tick({false, 0, 0, false, 0, true});
    ++p.counters.cycles;
}

const TextureStatistics& TextureUnit::statistics() const { return impl_->counters; }

} // namespace nano_gpu::hw
