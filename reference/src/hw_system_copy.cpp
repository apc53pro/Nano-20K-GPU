#include "nano_gpu/hw/system_copy.hpp"

#include "nano_gpu/hw/sdram.hpp"
#include "nano_gpu/hw/tile_pipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace nano_gpu::hw {
namespace {

void validate_image(const TextureDescriptor& image) {
    const auto extent = static_cast<std::uint64_t>(image.pitch_bytes) * image.height;
    if (image.width == 0 || image.width > 1024 || image.height == 0 || image.height > 1024 ||
        image.pitch_bytes < image.width * 2u || (image.pitch_bytes & 3u) != 0 ||
        image.base < heap_base || (image.base & 3u) != 0 || extent > memory_bytes ||
        static_cast<std::uint64_t>(image.base) + extent > memory_bytes) {
        throw std::invalid_argument("system copy image is outside the aligned SDRAM heap");
    }
}

void validate_command(const CopyCommand& command) {
    if (command.source < 0 || command.source > 1 || command.target < 0 || command.target > 1 ||
        command.width < 0 || command.height < 0 || (command.image_source && command.source != 0)) {
        throw std::invalid_argument("invalid system copy command");
    }
    if (command.image_source) validate_image(command.image);
    else if (command.image != TextureDescriptor{})
        throw std::invalid_argument("system framebuffer copy contains image metadata");
}

} // namespace

struct SystemCopy::Impl {
    enum class State {
        Idle, Invalidate, Address, ReadRequest, ReadWait, ReadFill,
        WritePrime, WriteFill, WriteSubmit, WriteWait, RowNext
    };

    State state = State::Idle;
    CopyStatistics counters;
    unsigned width = 0, height = 0, completed_rows = 0;
    bool reverse_rows = false;
    bool pending = false;
    std::uint32_t source_pitch = 0;
    std::uint32_t source_row = 0, destination_row = 0;
    std::uint32_t source_y_bits = 0, destination_y_bits = 0;
    std::uint32_t source_shifted_pitch = 0, destination_shifted_pitch = 0;
    unsigned address_bit = 0;

    std::uint32_t read_address = 0;
    unsigned read_words_remaining = 0, read_pixels = 0;
    MemoryResponse response;
    unsigned response_word = 0, response_lane = 0;
    MemoryRequest write_request;
    unsigned written_pixels = 0, group_pixels = 0, captured_pixels = 0;
    std::uint32_t next_tag = 1, expected_tag = 0;
    unsigned expected_words = 0;

    void start_read_row() {
        read_address = source_row & ~3u;
        read_words_remaining = ((source_row & 2u) + 2u * width + 3u) >> 2;
        read_pixels = written_pixels = 0;
        state = State::ReadRequest;
    }
};

SystemCopy::SystemCopy() : impl_(std::make_unique<Impl>()) {}
SystemCopy::~SystemCopy() = default;
SystemCopy::SystemCopy(SystemCopy&&) noexcept = default;
SystemCopy& SystemCopy::operator=(SystemCopy&&) noexcept = default;

void SystemCopy::begin(const CopyCommand& command) {
    if (busy()) throw std::logic_error("system copy command requires an idle engine");
    validate_command(command);
    // Validation/clipping uses widened host arithmetic to establish legal
    // register ranges. The timed address generator below performs the actual
    // variable-stride Y multiplication through ten shift/add edges.
    const std::int64_t source_width = command.image_source ? command.image.width : screen_width;
    const std::int64_t source_height = command.image_source ? command.image.height : screen_height;
    const auto sx = static_cast<std::int64_t>(command.source_origin.x);
    const auto sy = static_cast<std::int64_t>(command.source_origin.y);
    const auto dx = static_cast<std::int64_t>(command.destination_origin.x);
    const auto dy = static_cast<std::int64_t>(command.destination_origin.y);
    const auto x0 = std::max({std::int64_t{0}, -sx, -dx});
    const auto y0 = std::max({std::int64_t{0}, -sy, -dy});
    const auto x1 = std::min({static_cast<std::int64_t>(command.width), source_width - sx,
                             static_cast<std::int64_t>(screen_width) - dx});
    const auto y1 = std::min({static_cast<std::int64_t>(command.height), source_height - sy,
                             static_cast<std::int64_t>(screen_height) - dy});
    Impl next;
    next.counters.cycles = 1;
    if (x0 >= x1 || y0 >= y1) { *impl_ = next; return; }
    next.width = static_cast<unsigned>(x1 - x0);
    next.height = static_cast<unsigned>(y1 - y0);
    next.reverse_rows = !command.image_source && command.source == command.target && dy > sy;
    next.source_pitch = command.image_source ? command.image.pitch_bytes : screen_width * 2u;
    const auto source_base = command.image_source ? command.image.base :
        color_buffer_base(static_cast<unsigned>(command.source));
    next.source_row = source_base + static_cast<std::uint32_t>(sx + x0) * 2u;
    next.destination_row = color_buffer_base(static_cast<unsigned>(command.target)) +
                           static_cast<std::uint32_t>(dx + x0) * 2u;
    const auto first_row = next.reverse_rows ? next.height - 1u : 0u;
    next.source_y_bits = static_cast<std::uint32_t>(sy + y0) + first_row;
    next.destination_y_bits = static_cast<std::uint32_t>(dy + y0) + first_row;
    next.source_shifted_pitch = next.source_pitch;
    next.destination_shifted_pitch = screen_width * 2u;
    next.state = Impl::State::Invalidate;
    *impl_ = next;
}

bool SystemCopy::busy() const { return impl_->state != Impl::State::Idle; }
bool SystemCopy::memory_pending() const { return impl_->pending; }

void SystemCopy::reset() {
    if (memory_pending()) throw std::logic_error("drain the copy SDRAM response before reset");
    *impl_ = Impl{};
    impl_->counters.cycles = 1;
}

const CopyStatistics& SystemCopy::statistics() const { return impl_->counters; }

void SystemCopy::tick(SdramController& memory, OpaqueTile& tile, bool enable) {
    auto& p = *impl_;
    ++p.counters.cycles;
    if (!enable) { tile.tick(false); return; }
    using State = Impl::State;
    switch (p.state) {
    case State::Idle:
        tile.idle_edge();
        break;
    case State::Invalidate:
        tile.invalidate_texture();
        p.state = State::Address;
        break;
    case State::Address:
        // Two selected 32-bit adders run in parallel; no native wide multiply
        // or untimed memory access is hidden in row-address generation.
        if ((p.source_y_bits & 1u) != 0) p.source_row += p.source_shifted_pitch;
        if ((p.destination_y_bits & 1u) != 0) p.destination_row += p.destination_shifted_pitch;
        p.source_y_bits >>= 1;
        p.destination_y_bits >>= 1;
        if (++p.address_bit == 10) p.start_read_row();
        else {
            p.source_shifted_pitch <<= 1;
            p.destination_shifted_pitch <<= 1;
        }
        tile.idle_edge();
        break;
    case State::ReadRequest: {
        MemoryRequest request;
        request.address = p.read_address;
        request.words = std::min(memory_burst_words, p.read_words_remaining);
        request.tag = p.next_tag;
        if (memory.submit(MemoryClient::Copy, request)) {
            p.pending = true;
            p.expected_tag = p.next_tag++;
            p.expected_words = request.words;
            p.state = State::ReadWait;
        }
        tile.idle_edge();
        break;
    }
    case State::ReadWait:
        if (memory.response_ready(MemoryClient::Copy)) {
            p.response = memory.take_response(MemoryClient::Copy);
            p.pending = false;
            if (p.response.tag != p.expected_tag || p.response.words != p.expected_words)
                throw std::logic_error("system copy source response does not match request");
            p.response_word = 0;
            p.response_lane = p.read_pixels == 0 && (p.source_row & 2u) != 0 ? 1u : 0u;
            p.state = State::ReadFill;
        }
        tile.idle_edge();
        break;
    case State::ReadFill: {
        const auto value = static_cast<std::uint16_t>(
            p.response.data[p.response_word] >> (p.response_lane << 4));
        tile.copy_buffer_write(static_cast<std::uint16_t>(p.read_pixels), value);
        ++p.read_pixels;
        if (p.read_pixels == p.width) p.state = State::WritePrime;
        else if (++p.response_lane == 2) {
            p.response_lane = 0;
            if (++p.response_word == p.response.words) {
                p.read_words_remaining -= p.response.words;
                p.read_address += p.response.words * 4u;
                p.state = State::ReadRequest;
            }
        }
        break;
    }
    case State::WritePrime: {
        // One bounded 16x32 staging request receives at most 32 pixels.
        // The first odd destination starts in its high half-word, leaving
        // that first low half protected by the SDRAM byte mask.
        const auto address = p.destination_row + p.written_pixels * 2u;
        const auto first_lane = (address & 2u) != 0 ? 1u : 0u;
        p.group_pixels = std::min(2u * memory_burst_words - first_lane, p.width - p.written_pixels);
        p.captured_pixels = 0;
        p.write_request = {};
        p.write_request.write = true;
        p.write_request.address = address & ~3u;
        p.write_request.words = (first_lane + p.group_pixels + 1u) >> 1;
        p.write_request.tag = p.next_tag;
        tile.copy_buffer_read_request(static_cast<std::uint16_t>(p.written_pixels));
        p.state = State::WriteFill;
        break;
    }
    case State::WriteFill: {
        // Capture the OLD registered BSRAM result, then launch at most one
        // following read on this edge. Newly produced outputs are not used.
        const auto value = tile.copy_buffer_read_result();
        const auto pixel = p.written_pixels + p.captured_pixels;
        const auto address = p.destination_row + pixel * 2u;
        const auto word = (address - p.write_request.address) >> 2;
        const bool high_lane = (address & 2u) != 0;
        p.write_request.data[word] |= static_cast<std::uint32_t>(value) << (high_lane ? 16 : 0);
        p.write_request.masks[word] |= static_cast<std::uint8_t>(high_lane ? 0xcu : 0x3u);
        if (++p.captured_pixels == p.group_pixels) {
            tile.idle_edge();
            p.state = State::WriteSubmit;
        } else tile.copy_buffer_read_request(static_cast<std::uint16_t>(pixel + 1u));
        break;
    }
    case State::WriteSubmit:
        if (memory.submit(MemoryClient::Copy, p.write_request)) {
            p.pending = true;
            p.expected_tag = p.next_tag++;
            p.expected_words = p.write_request.words;
            p.state = State::WriteWait;
        }
        tile.idle_edge();
        break;
    case State::WriteWait:
        if (memory.response_ready(MemoryClient::Copy)) {
            const auto response = memory.take_response(MemoryClient::Copy);
            p.pending = false;
            if (response.tag != p.expected_tag || response.words != p.expected_words)
                throw std::logic_error("system copy destination response does not match request");
            p.counters.words += p.group_pixels;
            p.written_pixels += p.group_pixels;
            p.state = p.written_pixels == p.width ? State::RowNext : State::WritePrime;
        }
        tile.idle_edge();
        break;
    case State::RowNext:
        tile.idle_edge();
        if (++p.completed_rows == p.height) p.state = State::Idle;
        else {
            if (p.reverse_rows) {
                p.source_row -= p.source_pitch;
                p.destination_row -= screen_width * 2u;
            } else {
                p.source_row += p.source_pitch;
                p.destination_row += screen_width * 2u;
            }
            p.start_read_row();
        }
        break;
    }
}

} // namespace nano_gpu::hw
