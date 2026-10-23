#include "nano_gpu/hw/perspective.hpp"

#include <limits>
#include <stdexcept>

namespace nano_gpu::hw {

void PerspectiveDivider::start(std::int64_t attribute, std::int64_t reciprocal) {
    if (busy()) throw std::logic_error("perspective divider already has an active request");
    if (reciprocal < (std::int64_t{1} << 29) || reciprocal > (std::int64_t{1} << 37))
        throw std::invalid_argument("sample reciprocal W outside supported domain");

    // Unsigned subtraction forms abs(INT64_MIN) without signed overflow.
    const auto bits = static_cast<std::uint64_t>(attribute);
    negative_ = attribute < 0;
    magnitude_ = negative_ ? std::uint64_t{0} - bits : bits;
    divisor_ = static_cast<std::uint64_t>(reciprocal);
    remainder_ = 0;
    quotient_ = 0;
    bit_ = numerator_bits - 1;
    elapsed_edges_ = 1;
    overflow_ = false;
    result_valid_ = false;
    state_ = State::Divide;
}

void PerspectiveDivider::tick(bool enable) {
    if (!enable || state_ == State::Idle) return;
    ++elapsed_edges_;

    if (state_ == State::Divide) {
        const auto incoming_bit = bit_ >= 26 ? ((magnitude_ >> (bit_ - 26)) & 1u) : 0u;
        // Old remainder < divisor <= 2^37, so this shift fits 38 bits.
        remainder_ = (remainder_ << 1) | incoming_bit;
        if (remainder_ >= divisor_) {
            remainder_ -= divisor_;
            if (bit_ >= 32) overflow_ = true;
            else quotient_ |= std::uint32_t{1} << bit_;
        }
        if (bit_ == 0) state_ = State::Round;
        else --bit_;
        return;
    }

    std::uint64_t rounded = quotient_;
    const auto doubled_remainder = remainder_ << 1;
    if (doubled_remainder > divisor_ ||
        (doubled_remainder == divisor_ && (quotient_ & 1u))) {
        ++rounded;
    }
    const std::uint64_t limit = negative_ ? (std::uint64_t{1} << 31)
                                         : (std::uint64_t{1} << 31) - 1;
    state_ = State::Idle;
    if (overflow_ || rounded > limit) {
        result_valid_ = false;
        throw std::overflow_error("reconstructed UV does not fit signed Q16");
    }
    if (negative_ && rounded == (std::uint64_t{1} << 31)) {
        result_ = std::numeric_limits<std::int32_t>::min();
    } else {
        const auto narrowed = static_cast<std::int32_t>(rounded);
        result_ = negative_ ? -narrowed : narrowed;
    }
    result_valid_ = true;
}

std::int32_t PerspectiveDivider::result() const {
    if (!result_valid_) throw std::logic_error("perspective divider result is not valid");
    return result_;
}

void PerspectiveDivider::reset() {
    state_ = State::Idle;
    magnitude_ = 0;
    divisor_ = 0;
    remainder_ = 0;
    quotient_ = 0;
    result_ = 0;
    bit_ = numerator_bits - 1;
    elapsed_edges_ = 0;
    negative_ = false;
    overflow_ = false;
    result_valid_ = false;
}

} // namespace nano_gpu::hw
