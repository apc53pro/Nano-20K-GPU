#pragma once

#include "nano_gpu/reference.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace nano_gpu::hw {

class SdramController;

struct ScanoutConfig {
    unsigned pixel_divider = 4;
    unsigned horizontal_front_porch = 16;
    unsigned horizontal_sync = 96;
    unsigned horizontal_back_porch = 48;
    unsigned vertical_front_porch = 10;
    unsigned vertical_sync = 2;
    unsigned vertical_back_porch = 33;
    // Power of two, 64..2048 RGB565 words. Two physical SDPB 1024x16 banks
    // remain allocated; smaller settings select a stress-test FIFO limit.
    unsigned fifo_pixels = 2048;
};

struct ScanoutStatistics {
    std::uint64_t cycles = 0;
    std::uint64_t pixel_ticks = 0;
    std::uint64_t frames = 0;
    std::uint64_t completed_frames = 0;
    std::uint64_t active_pixels = 0;
    std::uint64_t displayed_pixels = 0;
    std::uint64_t startup_black_pixels = 0;
    std::uint64_t underflows = 0;
    std::uint64_t underflow_pixels = 0;
    std::uint64_t read_requests = 0;
    std::uint64_t stale_responses = 0;
    std::uint64_t framebuffer_swaps = 0;
    unsigned fifo_high_water = 0;
};

// Standard 640x480 scanout. With the default 100.8-MHz common clock, a divide
// by four pixel enable and 800x525 timing yield exactly 60 Hz. HDMI/TMDS
// encoding/electrical signaling is intentionally outside this model.
//
// Pixel data comes only through shared SDRAM reads, a bounded 16x32 response
// staging register and two selected SDPB 1024x16 FIFO banks. A synchronous
// memory result crosses a separate capture edge before pixel consumption.
// SDRAM itself is ticked by the common system scheduler, never by this block.
class Scanout {
public:
    explicit Scanout(ScanoutConfig config = {});
    ~Scanout();
    Scanout(const Scanout&) = delete;
    Scanout& operator=(const Scanout&) = delete;
    Scanout(Scanout&&) noexcept;
    Scanout& operator=(Scanout&&) noexcept;

    void tick(SdramController& memory);
    // Timing remains active while memory is unavailable. Enable waits for a
    // vertical blank before prefetching; no uninitialized memory is displayed.
    void set_memory_ready(bool ready);
    // A live swap is accepted only on the established vertical-blank pulse.
    // Before enable, either valid physical color ID may be selected.
    void set_front(int id);
    int front_id() const;
    bool vertical_blank_pulse() const;
    bool frame_pulse() const;
    std::uint64_t frame_count() const;
    unsigned horizontal_position() const;
    unsigned vertical_position() const;
    unsigned fifo_level() const;
    bool horizontal_sync() const;
    bool vertical_sync() const;
    const ScanoutStatistics& statistics() const;
    const ScanoutConfig& config() const;

    // Host-only waveform/image observations; these vectors do not provide a
    // device memory path. Underflow blanks the remainder of a frame and
    // resynchronizes at vertical blank so stale pixels cannot shift alignment.
    const std::vector<std::uint16_t>& completed_frame() const;
    int completed_frame_front_id() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nano_gpu::hw
