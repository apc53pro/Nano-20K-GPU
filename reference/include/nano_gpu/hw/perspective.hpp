#pragma once

#include <cstdint>

namespace nano_gpu::hw {

// Explicit LUT/register restoring divider, not an assumed Gowin DSP operation.
// Reconstructs signed Q16 UV as RNE((attribute << 26) / reciprocal), where the
// shift is a wired scale conversion from the M0 Q27/Q37 prepared planes.
// The logical numerator has 90 bits; its lower 26 bits are constant zero.
// Divisor/remainder arithmetic is bounded to 38 bits and the quotient has a
// 32-bit result register plus an overflow flag. No host divide, wide multiply,
// floating point, or __int128 is used by the modeled datapath.
class PerspectiveDivider {
public:
    static constexpr unsigned numerator_bits = 90;
    static constexpr unsigned divisor_bits = 38;
    static constexpr unsigned enabled_edge_latency = numerator_bits + 2;

    // Capture operands on one modeled rising edge. A completed result may be
    // replaced; an active request may not. Invalid requests preserve old state.
    // reciprocal must be in the M0 sample domain [2^29, 2^37].
    void start(std::int64_t attribute, std::int64_t reciprocal);

    // Ninety enabled edges consume numerator bits; one further edge performs
    // ties-to-even rounding/sign application. CE-low holds every register and
    // does not advance elapsed_edges(). Idle ticks have no effect.
    // Result overflow retires the request, invalidates its result, and throws.
    void tick(bool enable = true);

    bool busy() const { return state_ != State::Idle; }
    bool result_valid() const { return result_valid_; }
    std::int32_t result() const;

    // Synchronous controller reset/cancel; no result remains valid afterwards.
    // Reset itself is accounted by the surrounding block's common clock.
    void reset();
    std::uint32_t elapsed_edges() const { return elapsed_edges_; }

private:
    enum class State { Idle, Divide, Round };
    State state_ = State::Idle;
    std::uint64_t magnitude_ = 0;
    std::uint64_t divisor_ = 0;
    std::uint64_t remainder_ = 0;
    std::uint32_t quotient_ = 0;
    std::int32_t result_ = 0;
    unsigned bit_ = numerator_bits - 1;
    std::uint32_t elapsed_edges_ = 0;
    bool negative_ = false;
    bool overflow_ = false;
    bool result_valid_ = false;
};

} // namespace nano_gpu::hw
