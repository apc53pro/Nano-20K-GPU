#include "nano_gpu/hw/mult18x18.hpp"

#include <stdexcept>

namespace nano_gpu::hw {

void Mult18x18::tick(const Mult18Cycle& cycle) {
    if (cycle.reset) {
        a_ = 0;
        b_ = 0;
        pipeline_ = 0;
        output_ = 0;
        inputs_known_ = true;
        pipeline_known_ = true;
        output_known_ = true;
        return;
    }
    if (!cycle.ce) return;

    constexpr std::int32_t minimum = -(std::int32_t{1} << (operand_bits - 1));
    constexpr std::int32_t maximum = (std::int32_t{1} << (operand_bits - 1)) - 1;
    if (cycle.a < minimum || cycle.a > maximum ||
        cycle.b < minimum || cycle.b > maximum) {
        throw std::out_of_range("MULT18X18 operand exceeds signed 18-bit range");
    }

    // The complete 18x18 signed result fits in the physical 36-bit product.
    // Widen before multiplying to avoid a host 32-bit intermediate overflow.
    const std::int64_t next_product = static_cast<std::int64_t>(a_) * b_;
    output_ = pipeline_;
    output_known_ = pipeline_known_;
    pipeline_ = next_product;
    pipeline_known_ = inputs_known_;
    a_ = cycle.a;
    b_ = cycle.b;
    inputs_known_ = true;
}

} // namespace nano_gpu::hw
