#pragma once

#include <cstdint>

namespace nano_gpu::hw {

struct Mult18Cycle {
    std::int32_t a = 0;
    std::int32_t b = 0;
    bool ce = true;
    bool reset = false;
};

// Selected GW2AR MULT18X18 subset: static signed operands, parallel inputs,
// AREG=BREG=PIPE_REG=OUT_REG=1, synchronous active-high reset and shared CE.
// Sources: SUG949-2.2E section 10.1.1 (signed 18x18 example), and
// UG287-1.4E section 4.2.2. Dynamic sign control and shift routing are outside
// this model. Each tick represents one rising edge; all stages use old state.
class Mult18x18 {
public:
    static constexpr unsigned operand_bits = 18;
    static constexpr unsigned product_bits = 36;
    static constexpr unsigned enabled_edge_latency = 3;

    // Out-of-width host operands are errors only when captured. Reset clears
    // every stage before considering CE or operand values; CE-low holds state.
    void tick(const Mult18Cycle& cycle);

    // The numeric placeholder is not a known hardware value unless
    // output_known() is true. Power-up register contents are not assumed zero.
    std::int64_t product() const { return output_; }
    bool output_known() const { return output_known_; }

private:
    std::int32_t a_ = 0;
    std::int32_t b_ = 0;
    std::int64_t pipeline_ = 0;
    std::int64_t output_ = 0;
    bool inputs_known_ = false;
    bool pipeline_known_ = false;
    bool output_known_ = false;
};

} // namespace nano_gpu::hw
