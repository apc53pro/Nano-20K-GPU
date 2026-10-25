#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nano_gpu::hw {

// Selected GW2AR-18C SDPB mode, not a generic dual-port RAM:
//   BIT_WIDTH_0=BIT_WIDTH_1=16, READ_MODE=0 (BYPASS), RESET_MODE="SYNC".
// A is a write-only port; B is a synchronous read-only port. CLKA=CLKB.
// RESETA is tied low; CEA=write_enable; CEB=read_enable || reset.
// BLK_SEL_0=BLK_SEL_1=BLKSELA=BLKSELB=0; INIT_RAM_00..3F=0.
// OCE has no effect in BYPASS mode. Only full-word writes are exposed.
// Sources: UG285-1.4E (2025-10-17), section 3.3 pp.24-28, chapter 4 pp.35-36:
// https://cdn.gowinsemi.com.cn/UG285E.pdf
// Reset has priority over read enable; the equivalent clocked coding style is
// shown in SUG949-2.2E (2026-06-17), section 8.1.2:
// https://cdn.gowinsemi.com.cn/SUG949E.pdf
struct BsramCycle {
    bool write_enable = false;
    std::uint16_t write_address = 0;
    std::uint16_t write_data = 0;
    bool read_enable = false;
    std::uint16_t read_address = 0;
    bool reset = false;
};

class Sdp1kx16 {
public:
    static constexpr std::size_t words = 1024;
    static constexpr unsigned word_bits = 16;
    static constexpr unsigned read_latency = 1;

    Sdp1kx16() = default;
    // A selected constant INIT_RAM image, applied by FPGA configuration rather
    // than by runtime writes. Output still starts unknown and reset preserves
    // these words exactly as it preserves the zero-initialized default image.
    explicit Sdp1kx16(const std::array<std::uint16_t, words>& initial) : contents_(initial) {}

    // Apply one common rising edge. Illegal requests throw before any mutation.
    // Cross-port same-address read/write is forbidden, not assigned a RAM value.
    // Reset zeros only the output latch. It never clears stored words.
    void tick(const BsramCycle& cycle);

    // The model requires a read or reset before observing the output latch.
    bool output_known() const { return output_known_; }
    std::uint16_t output() const;

    // Native AD[13:0] packing from UG285 section 3.3, p.28. Low write bits enable
    // both bytes: ADA={word[9:0],2'b00,2'b11}; ADB={word[9:0],4'b0000}.
    static std::uint16_t write_bus_address(std::uint16_t word);
    static std::uint16_t read_bus_address(std::uint16_t word);

private:
    std::array<std::uint16_t, words> contents_{};
    std::uint16_t output_ = 0;
    bool output_known_ = false;
};

} // namespace nano_gpu::hw
