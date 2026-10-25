#pragma once

#include <cstdint>
#include <memory>

namespace nano_gpu::hw {

// One shared reciprocal reconstructs both U and V. Every multiplication is
// made from selected registered signed18x18 primitives; positive wide operands
// use17-bit limbs. The approximate quotient receives bounded exact remainder
// correction, preserving canonical signedQ16 round-to-nearest-even results.
// Four native SDPB banks contain the fixed256-entry linear seed ROM.
class PerspectivePipeline {
public:
    static constexpr unsigned enabled_edge_latency = 26;
    static constexpr unsigned mult18_equivalents = 40;
    static constexpr unsigned seed_bsram_blocks = 4;
    static constexpr unsigned context_bits = 6;

    struct Input {
        std::int64_t u = 0, v = 0, q = std::int64_t{1} << 29;
        std::uint8_t tag = 0;
        bool valid = false;
    };
    struct Output {
        std::int32_t u = 0, v = 0;
        std::uint8_t tag = 0;
        bool valid = false;
        bool overflow = false;
        bool invalid_q = false;
    };

    PerspectivePipeline();
    ~PerspectivePipeline();
    PerspectivePipeline(const PerspectivePipeline&) = delete;
    PerspectivePipeline& operator=(const PerspectivePipeline&) = delete;
    PerspectivePipeline(PerspectivePipeline&&) noexcept;
    PerspectivePipeline& operator=(PerspectivePipeline&&) noexcept;

    // One common edge. A valid input may be captured on every enabled edge.
    // Disabled edges hold every control, ROM and DSP register and output.
    // Consumers sample the OLD registered output before calling tick().
    // Invalid q or overflowing UV retires with a flag, never a wrapped value.
    void tick(const Input& input, bool enable = true);
    const Output& output() const;
    bool busy() const;
    // Synchronous cancellation, clocked by the enclosing controller's reset
    // edge. ROM contents persist; all pipeline-valid bits are cleared.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nano_gpu::hw
