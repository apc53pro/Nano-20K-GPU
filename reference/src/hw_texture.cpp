#include "nano_gpu/hw/texture.hpp"

#include "nano_gpu/hw/bsram.hpp"
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
        Refill, CacheRead, Capture
    };

    Sdp1kx16 bank;
    std::array<std::uint32_t, line_count> tags{};
    std::array<bool, line_count> valid{};
    std::shared_ptr<const Image> texture;
    TextureDescriptor descriptor;
    SdramController* external_memory = nullptr;
    bool external_request_pending = false;
    unsigned external_words = 0;
    std::array<std::uint32_t, line_words / 2> external_line{};
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

    void require_idle() const {
        if (state != State::Idle)
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
};

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

bool TextureUnit::busy() const { return impl_->state != Impl::State::Idle; }
bool TextureUnit::external_memory_pending() const { return impl_->external_request_pending; }
bool TextureUnit::result_valid() const { return impl_->result_valid; }

std::uint16_t TextureUnit::result() const {
    if (!impl_->result_valid) throw std::logic_error("hardware texture result is not valid");
    return impl_->result;
}

void TextureUnit::tick(bool enable) {
    auto& p = *impl_;
    ++p.counters.cycles;
    if (!enable) {
        p.bank.tick({});
        ++p.counters.stalls;
        return;
    }

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
    case Impl::State::ResponseWait:
        if (--p.response_wait == 0) p.state = Impl::State::Refill;
        break;
    case Impl::State::ExternalRequest: {
        // The allocation is four-byte aligned, and each cache line holds an
        // integral number of 32-bit words. A final short line is never read
        // past that allocation; only its unused cache tail is padded with zero.
        const auto allocation_words = static_cast<std::uint64_t>(p.descriptor.pitch_bytes) *
                                      p.descriptor.height / 4u;
        const auto first_word = p.line_base >> 1;
        if (first_word >= allocation_words)
            throw std::logic_error("hardware texture refill starts outside its allocation");
        MemoryRequest request;
        request.address = p.descriptor.base + (p.line_base << 1);
        request.words = static_cast<unsigned>(std::min<std::uint64_t>(
            line_words / 2, allocation_words - first_word));
        request.tag = p.line_base;
        if (p.external_memory->submit(MemoryClient::Texture, request)) {
            p.external_request_pending = true;
            p.external_words = request.words;
            p.state = Impl::State::ExternalWait;
        } else ++p.counters.memory_wait_cycles;
        break;
    }
    case Impl::State::ExternalWait:
        if (p.external_memory->response_ready(MemoryClient::Texture)) {
            const auto response = p.external_memory->take_response(MemoryClient::Texture);
            p.external_request_pending = false;
            if (response.tag != p.line_base || response.words != p.external_words ||
                response.words == 0 || response.words > p.external_line.size())
                throw std::logic_error("hardware texture SDRAM response does not match its request");
            p.external_line.fill(0);
            for (unsigned word = 0; word < response.words; ++word)
                p.external_line[word] = response.data[word];
            p.state = Impl::State::Refill;
        } else ++p.counters.memory_wait_cycles;
        break;
    case Impl::State::Refill:
        memory.write_enable = true;
        memory.write_address = static_cast<std::uint16_t>((p.slot << 4) | p.refill_word);
        memory.write_data = p.external_memory ? static_cast<std::uint16_t>(
            p.external_line[p.refill_word >> 1] >> ((p.refill_word & 1u) << 4)) :
            p.backing_word(p.line_base + p.refill_word);
        ++p.counters.refill_words;
        if (++p.refill_word == line_words) {
            p.tags[p.slot] = p.tag;
            p.valid[p.slot] = true;
            p.state = Impl::State::CacheRead;
        }
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
    p.external_line.fill(0);
    p.valid.fill(false);
    p.tags.fill(0);
    p.result_valid = false;
    p.scratch_read_valid = false;
    p.result = 0;
    p.address = p.shifted_stride = p.y_bits = 0;
    p.address_bit = p.slot = p.word_in_line = p.response_wait = p.refill_word = 0;
    p.line_base = p.tag = 0;
    p.counters = {};
    p.bank.tick({false, 0, 0, false, 0, true});
    ++p.counters.cycles;
}

const TextureStatistics& TextureUnit::statistics() const { return impl_->counters; }

} // namespace nano_gpu::hw
