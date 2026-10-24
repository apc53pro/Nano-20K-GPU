#include "nano_gpu/hw/scanout.hpp"

#include "nano_gpu/hw/bsram.hpp"
#include "nano_gpu/hw/sdram.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace nano_gpu::hw {
namespace {

constexpr unsigned frame_pixels = screen_width * screen_height;
constexpr std::uint32_t color_buffer_bytes = frame_pixels * 2u;

void validate_config(const ScanoutConfig& config) {
    if (config.pixel_divider == 0 || config.pixel_divider > 1024 ||
        config.fifo_pixels < 64 || config.fifo_pixels > 2048 ||
        (config.fifo_pixels & (config.fifo_pixels - 1u)) != 0 ||
        config.horizontal_front_porch > 4096 || config.horizontal_sync == 0 ||
        config.horizontal_sync > 4096 || config.horizontal_back_porch > 4096 ||
        config.vertical_front_porch > 4096 || config.vertical_sync == 0 ||
        config.vertical_sync > 4096 || config.vertical_back_porch > 4096) {
        throw std::invalid_argument("invalid bounded scanout timing/FIFO configuration");
    }
}

} // namespace

struct Scanout::Impl {
    explicit Impl(ScanoutConfig selected) : config(selected),
        observed_frame(frame_pixels), completed(frame_pixels) {
        validate_config(config);
        for (auto& bank : fifo) bank.tick({false, 0, 0, false, 0, true});
    }

    ScanoutConfig config;
    ScanoutStatistics counters;
    std::array<Sdp1kx16, 2> fifo;
    unsigned read_pointer = 0, write_pointer = 0, stored = 0;
    bool read_pending = false, output_valid = false;
    unsigned read_bank = 0;
    std::uint16_t output = 0;
    // The controller's two native slot banks retain the whole response.
    // Only one 32-bit output register bridges their synchronous read port
    // to the FIFO's 16-bit write port.
    std::uint32_t response_word = 0;
    unsigned response_words = 0, response_index = 0, response_lane = 0;
    bool response_active = false, response_read_pending = false, response_word_valid = false;
    bool memory_pending = false, pending_discard = false;
    unsigned pending_pixels = 0;
    std::uint32_t generation = 0;
    unsigned next_fetch_pixel = 0;
    unsigned horizontal = 0, vertical = 0, pixel_phase = 0;
    bool blank_pulse = false, wrapped_pulse = false;
    bool memory_ready = false, streaming = false, frame_black = false;
    int front = 0, active_frame_front = -1, completed_front = -1;
    bool active_frame_ready = false;
    std::vector<std::uint16_t> observed_frame, completed;

    unsigned horizontal_total() const {
        return screen_width + config.horizontal_front_porch + config.horizontal_sync +
               config.horizontal_back_porch;
    }
    unsigned vertical_total() const {
        return screen_height + config.vertical_front_porch + config.vertical_sync +
               config.vertical_back_porch;
    }
    unsigned level() const {
        return stored + static_cast<unsigned>(read_pending) + static_cast<unsigned>(output_valid);
    }
    void flush_stream() {
        read_pointer = write_pointer = stored = 0;
        read_pending = output_valid = false;
        response_words = response_index = response_lane = 0;
        response_active = response_word_valid = false;
        // A staged controller read cannot be retracted. Its output is drained
        // before the old slot may be released, including on a live swap.
        next_fetch_pixel = 0;
        if (memory_pending) pending_discard = true;
        ++generation;
    }
    void enter_blank() {
        completed = observed_frame; // Diagnostic observer, outside device state.
        completed_front = active_frame_ready ? active_frame_front : -1;
        ++counters.completed_frames;
        blank_pulse = true;
        flush_stream();
        frame_black = false;
        streaming = memory_ready;
    }
    void pixel_edge() {
        ++counters.pixel_ticks;
        if (horizontal < screen_width && vertical < screen_height) {
            ++counters.active_pixels;
            std::uint16_t pixel = 0;
            if (!active_frame_ready || !memory_ready) {
                ++counters.startup_black_pixels;
            } else if (frame_black) {
                ++counters.underflow_pixels;
            } else if (output_valid) {
                pixel = output;
                output_valid = false;
                ++counters.displayed_pixels;
            } else {
                ++counters.underflows;
                ++counters.underflow_pixels;
                frame_black = true;
                flush_stream();
            }
            observed_frame[static_cast<std::size_t>(vertical) * screen_width + horizontal] = pixel;
        }
        if (++horizontal == horizontal_total()) {
            horizontal = 0;
            if (++vertical == screen_height) enter_blank();
            if (vertical == vertical_total()) {
                vertical = 0;
                wrapped_pulse = true;
                ++counters.frames;
                active_frame_front = front;
                active_frame_ready = streaming && memory_ready;
            }
        }
    }
};

Scanout::Scanout(ScanoutConfig config) : impl_(std::make_unique<Impl>(config)) {}
Scanout::~Scanout() = default;
Scanout::Scanout(Scanout&&) noexcept = default;
Scanout& Scanout::operator=(Scanout&&) noexcept = default;

void Scanout::tick(SdramController& memory) {
    auto& p = *impl_;
    ++p.counters.cycles;
    p.blank_pulse = p.wrapped_pulse = false;
    // Consume the OLD output register before any capture on this common edge.
    if (++p.pixel_phase == p.config.pixel_divider) {
        p.pixel_phase = 0;
        p.pixel_edge();
    }

    std::array<BsramCycle, 2> ports{};
    if (p.read_pending && !p.output_valid) {
        // Observe only the register established by a prior BSRAM edge.
        p.output = p.fifo[p.read_bank].output();
        p.output_valid = true;
        p.read_pending = false;
    }
    if (p.streaming && !p.frame_black && !p.read_pending && p.stored != 0) {
        p.read_bank = p.read_pointer >> 10;
        auto& read = ports[p.read_bank];
        read.read_enable = true;
        read.read_address = static_cast<std::uint16_t>(p.read_pointer & 1023u);
        p.read_pointer = (p.read_pointer + 1) & (p.config.fifo_pixels - 1u);
        --p.stored;
        p.read_pending = true;
    }
    // Only a word captured on a prior edge feeds the FIFO write port.
    if (p.streaming && !p.frame_black && p.response_word_valid &&
        p.level() < p.config.fifo_pixels) {
        const auto pixel = static_cast<std::uint16_t>(p.response_word >> (p.response_lane * 16));
        auto& write = ports[p.write_pointer >> 10];
        write.write_enable = true;
        write.write_address = static_cast<std::uint16_t>(p.write_pointer & 1023u);
        write.write_data = pixel;
        p.write_pointer = (p.write_pointer + 1) & (p.config.fifo_pixels - 1u);
        ++p.stored;
        if (++p.response_lane == 2) {
            p.response_lane = 0;
            p.response_word_valid = false;
            if (++p.response_index == p.response_words) {
                memory.release_response(MemoryClient::Scanout);
                p.memory_pending = p.response_active = false;
            }
        }
    }
    for (unsigned bank = 0; bank < p.fifo.size(); ++bank) p.fifo[bank].tick(ports[bank]);

    if (p.memory_pending && (p.pending_discard || !p.streaming || p.frame_black) &&
        memory.response_ready(MemoryClient::Scanout)) {
        if (!p.response_read_pending || memory.response_word_valid(MemoryClient::Scanout)) {
            memory.release_response(MemoryClient::Scanout);
            p.memory_pending = p.pending_discard = p.response_active = false;
            p.response_read_pending = p.response_word_valid = false;
            ++p.counters.stale_responses;
        }
    } else if (p.response_read_pending && memory.response_word_valid(MemoryClient::Scanout)) {
        if (memory.response_word_offset(MemoryClient::Scanout) != p.response_index)
            throw std::logic_error("scanout synchronous slot read returned a different word");
        p.response_word = memory.response_word(MemoryClient::Scanout);
        p.response_read_pending = false;
        p.response_word_valid = true;
        p.response_lane = 0;
    }

    if (p.memory_pending && !p.pending_discard && !p.response_active &&
        memory.response_ready(MemoryClient::Scanout)) {
        const auto response = memory.response_info(MemoryClient::Scanout);
        if (response.words * 2 != p.pending_pixels || response.tag != p.generation)
            throw std::logic_error("scanout response does not match its outstanding read");
        p.response_words = response.words;
        p.response_index = p.response_lane = 0;
        p.response_active = true;
    }
    if (p.response_active && !p.pending_discard && !p.response_word_valid && !p.response_read_pending &&
        p.streaming && !p.frame_black) {
        if (memory.request_response_word(MemoryClient::Scanout, p.response_index))
            p.response_read_pending = true;
    }

    if (p.streaming && !p.frame_black && memory.initialized() && !p.memory_pending &&
        p.next_fetch_pixel < frame_pixels &&
        p.config.fifo_pixels - p.level() >= memory_burst_words * 2) {
        MemoryRequest request;
        request.address = static_cast<std::uint32_t>(p.front) * color_buffer_bytes + p.next_fetch_pixel * 2u;
        request.words = std::min(memory_burst_words, (frame_pixels - p.next_fetch_pixel) / 2);
        request.tag = p.generation;
        if (memory.submit_prepared(MemoryClient::Scanout, request)) {
            p.memory_pending = true;
            p.pending_discard = false;
            p.pending_pixels = request.words * 2;
            p.next_fetch_pixel += p.pending_pixels;
            ++p.counters.read_requests;
        }
    }
    p.counters.fifo_high_water = std::max(p.counters.fifo_high_water, p.level());
    if (p.level() > p.config.fifo_pixels)
        throw std::logic_error("scanout FIFO exceeded its configured capacity");
}

void Scanout::set_memory_ready(bool ready) {
    auto& p = *impl_;
    if (p.memory_ready == ready) return;
    p.memory_ready = ready;
    p.streaming = false;
    p.active_frame_ready = false;
    p.frame_black = false;
    p.flush_stream();
}

void Scanout::set_front(int id) {
    auto& p = *impl_;
    if (id != 0 && id != 1) throw std::out_of_range("scanout front buffer ID must be zero or one");
    if (p.memory_ready && !p.blank_pulse)
        throw std::logic_error("scanout front buffer can change only at vertical blank");
    if (p.front == id) return;
    p.front = id;
    p.flush_stream();
    ++p.counters.framebuffer_swaps;
}

int Scanout::front_id() const { return impl_->front; }
bool Scanout::vertical_blank_pulse() const { return impl_->blank_pulse; }
bool Scanout::frame_pulse() const { return impl_->wrapped_pulse; }
std::uint64_t Scanout::frame_count() const { return impl_->counters.frames; }
unsigned Scanout::horizontal_position() const { return impl_->horizontal; }
unsigned Scanout::vertical_position() const { return impl_->vertical; }
unsigned Scanout::fifo_level() const { return impl_->level(); }
bool Scanout::horizontal_sync() const {
    const auto& p = *impl_;
    const auto start = screen_width + p.config.horizontal_front_porch;
    return !(p.horizontal >= start && p.horizontal < start + p.config.horizontal_sync);
}
bool Scanout::vertical_sync() const {
    const auto& p = *impl_;
    const auto start = screen_height + p.config.vertical_front_porch;
    return !(p.vertical >= start && p.vertical < start + p.config.vertical_sync);
}
const ScanoutStatistics& Scanout::statistics() const { return impl_->counters; }
const ScanoutConfig& Scanout::config() const { return impl_->config; }
const std::vector<std::uint16_t>& Scanout::completed_frame() const { return impl_->completed; }
int Scanout::completed_frame_front_id() const { return impl_->completed_front; }

} // namespace nano_gpu::hw
