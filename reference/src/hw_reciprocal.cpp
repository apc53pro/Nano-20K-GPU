#include "nano_gpu/hw/reciprocal.hpp"

#include "nano_gpu/hw/bsram.hpp"
#include "nano_gpu/hw/mult18x18.hpp"
#include "nano_gpu/hw/reciprocal_seed.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nano_gpu::hw {
namespace {

// These fixed-width helpers represent adders, shifts and comparisons. All
// variable products below come from the40 modeled MULT18X18 instances.
struct U96 { std::array<std::uint32_t, 3> word{}; };

bool bit(const U96& value, unsigned at) {
    return at < 96 && ((value.word[at / 32] >> (at & 31u)) & 1u) != 0;
}
void add(U96& value, const U96& term) {
    std::uint64_t carry = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const auto sum = std::uint64_t{value.word[i]} + term.word[i] + carry;
        value.word[i] = static_cast<std::uint32_t>(sum); carry = sum >> 32;
    }
    if (carry != 0) throw std::logic_error("reciprocal partial-product adder exceeded96bits");
}
U96 shifted(std::uint64_t value, unsigned shift) {
    U96 result;
    for (unsigned i = 0; i < 64 && i + shift < 96; ++i)
        if (((value >> i) & 1u) != 0)
            result.word[(i + shift) / 32] |= std::uint32_t{1} << ((i + shift) & 31u);
    return result;
}
std::uint64_t rounded(const U96& value, unsigned shift) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (bit(value, i + shift)) result |= std::uint64_t{1} << i;
    for (unsigned i = shift + 64; i < 96; ++i)
        if (bit(value, i)) throw std::logic_error("reciprocal rounded shifter exceeded64bits");
    if (shift == 0) return result;
    bool sticky = false;
    for (unsigned i = 0; i + 1 < shift; ++i) sticky = sticky || bit(value, i);
    if (bit(value, shift - 1) && (sticky || (result & 1u) != 0)) ++result;
    return result;
}
std::uint64_t rounded(std::uint64_t value, unsigned shift) {
    return rounded(shifted(value, 0), shift);
}
std::int64_t signed_rounded(std::int64_t value, unsigned shift) {
    const auto bits = static_cast<std::uint64_t>(value);
    const auto magnitude = value < 0 ? std::uint64_t{0} - bits : bits;
    const auto result = rounded(magnitude, shift);
    return value < 0 ? -static_cast<std::int64_t>(result) : static_cast<std::int64_t>(result);
}
bool less(const U96& a, const U96& b) {
    for (unsigned i = 3; i-- != 0;)
        if (a.word[i] != b.word[i]) return a.word[i] < b.word[i];
    return false;
}
std::int64_t difference(const U96& a, const U96& b) {
    const bool negative = less(a, b);
    const auto& large = negative ? b : a;
    const auto& small = negative ? a : b;
    U96 result; std::uint64_t borrow = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const auto rhs = std::uint64_t{small.word[i]} + borrow;
        const auto lhs = std::uint64_t{large.word[i]};
        result.word[i] = static_cast<std::uint32_t>(lhs - rhs); borrow = lhs < rhs ? 1 : 0;
    }
    if (result.word[2] != 0 || (result.word[1] & 0x80000000u) != 0)
        throw std::logic_error("reciprocal quotient estimate exceeded bounded exact correction");
    const auto magnitude = std::uint64_t{result.word[0]} | (std::uint64_t{result.word[1]} << 32);
    return negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
}

std::array<std::uint16_t, Sdp1kx16::words> seed_image(unsigned bank) {
    std::array<std::uint16_t, Sdp1kx16::words> image{};
    for (unsigned i = 0; i < 256; ++i)
        image[i] = bank < 3 ? static_cast<std::uint16_t>(reciprocal_seed_base[i] >> (16 * bank)) :
            reciprocal_seed_gradient[i];
    return image;
}

} // namespace

struct PerspectivePipeline::Impl {
    // This aggregate is a software view of each stage's live signals. Dead
    // fields between operations are wires/aliases in RTL, not a second copy of
    // every field in every physical stage; synthesis supplies the exact FF use.
    struct Stage {
        bool valid = false, overflow = false, invalid_q = false;
        std::array<bool, 2> negative{};
        std::uint8_t tag = 0;
        unsigned exponent = 29;
        std::uint64_t q = std::uint64_t{1} << 29;
        std::uint64_t normalized = std::uint64_t{1} << 32;
        std::array<std::uint64_t, 2> magnitude{};
        std::uint64_t seed_base = 0, reciprocal = 0;
        std::uint16_t gradient = 0;
        std::int32_t delta = 0, error = 0;
        std::array<std::uint32_t, 2> quotient{};
        std::array<std::int64_t, 2> remainder{};
    };

    std::array<Stage, enabled_edge_latency> stages{};
    std::array<Mult18x18, mult18_equivalents> multipliers;
    std::array<Sdp1kx16, seed_bsram_blocks> rom{{Sdp1kx16(seed_image(0)), Sdp1kx16(seed_image(1)),
        Sdp1kx16(seed_image(2)), Sdp1kx16(seed_image(3))}};
    Output output;

    static Stage capture(const Input& input) {
        Stage s; s.valid = input.valid; s.tag = input.tag;
        if (!input.valid) return s;
        if (input.tag >= 64) throw std::out_of_range("perspective context tag exceeds6bits");
        s.invalid_q = input.q < (std::int64_t{1} << 29) || input.q > (std::int64_t{1} << 37);
        s.q = s.invalid_q ? std::uint64_t{1} << 29 : static_cast<std::uint64_t>(input.q);
        const std::array<std::int64_t, 2> attributes{{input.u, input.v}};
        for (unsigned i = 0; i < 2; ++i) {
            s.negative[i] = attributes[i] < 0;
            const auto bits = static_cast<std::uint64_t>(attributes[i]);
            s.magnitude[i] = s.negative[i] ? std::uint64_t{0} - bits : bits;
            // Stage0 rejects high input bits and captures at most43-bit
            // magnitude. The divisor-dependent bound follows on stage1,
            // avoiding a64-bit negate/limit/compare chain on one edge.
            s.overflow = s.overflow || (s.magnitude[i] >> 43) != 0;
        }
        if (s.overflow || s.invalid_q) s.magnitude = {};
        return s;
    }

    static void normalize(Stage& s) {
        if (!s.valid) return;
        for (const auto magnitude : s.magnitude)
            s.overflow = s.overflow || magnitude > (s.q << 5) + 1024;
        if (s.overflow || s.invalid_q) s.magnitude = {};
        s.exponent = 0;
        for (unsigned i = 0; i < 38; ++i) if (((s.q >> i) & 1u) != 0) s.exponent = i;
        s.normalized = s.exponent <= 32 ? s.q << (32 - s.exponent) : rounded(s.q, s.exponent - 32);
        // Normalization round-to-even may carry into the next power of two.
        if (s.normalized == (std::uint64_t{1} << 33)) { s.normalized >>= 1; ++s.exponent; }
    }

    void wide_ports(std::array<Mult18Cycle, mult18_equivalents>& ports, unsigned first,
                    std::uint64_t a, unsigned a_limbs, std::uint64_t b, unsigned b_limbs) const {
        for (unsigned i = 0; i < a_limbs; ++i)
            for (unsigned j = 0; j < b_limbs; ++j)
                ports[first + i * b_limbs + j] = {
                    static_cast<std::int32_t>((a >> (17 * i)) & 0x1ffffu),
                    static_cast<std::int32_t>((b >> (17 * j)) & 0x1ffffu), true, false};
    }
    U96 wide_result(unsigned first, unsigned a_limbs, unsigned b_limbs) const {
        U96 result;
        for (unsigned i = 0; i < a_limbs; ++i) {
            for (unsigned j = 0; j < b_limbs; ++j) {
                const auto& m = multipliers[first + i * b_limbs + j];
                if (!m.output_known() || m.product() < 0)
                    throw std::logic_error("reciprocal partial product is not established");
                add(result, shifted(static_cast<std::uint64_t>(m.product()), 17 * (i + j)));
            }
        }
        return result;
    }
    std::int64_t correction_product() const {
        std::int64_t result = 0;
        for (unsigned i = 0; i < 3; ++i) {
            const auto& m = multipliers[7 + i];
            if (!m.output_known()) throw std::logic_error("reciprocal Newton product is unknown");
            const auto term = m.product();
            const auto magnitude = static_cast<std::uint64_t>(term < 0 ? -term : term) << (17 * i);
            result += term < 0 ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
        }
        return result;
    }
};

PerspectivePipeline::PerspectivePipeline() : impl_(std::make_unique<Impl>()) { reset(); }
PerspectivePipeline::~PerspectivePipeline() = default;
PerspectivePipeline::PerspectivePipeline(PerspectivePipeline&&) noexcept = default;
PerspectivePipeline& PerspectivePipeline::operator=(PerspectivePipeline&&) noexcept = default;

void PerspectivePipeline::tick(const Input& input, bool enable) {
    auto& p = *impl_;
    if (!enable) return;
    auto next = p.stages;
    for (unsigned i = enabled_edge_latency - 1; i != 0; --i) next[i] = p.stages[i - 1];
    next[0] = Impl::capture(input);
    Impl::normalize(next[1]);
    std::array<Mult18Cycle, mult18_equivalents> ports;
    for (auto& port : ports) port = {0, 0, true, false};
    std::array<BsramCycle, seed_bsram_blocks> rom_ports{};

    if (next[2].valid) {
        const auto index = static_cast<std::uint16_t>((next[2].normalized - (std::uint64_t{1} << 32)) >> 24);
        // OLD stage1 normalization drives the residual rounder in parallel
        // with this edge's native ROM read; seed inputs are captured on3.
        const auto center = (std::int64_t{1} << 32) + (static_cast<std::int64_t>(2 * index + 1) << 23);
        next[2].delta = static_cast<std::int32_t>(signed_rounded(
            static_cast<std::int64_t>(next[2].normalized) - center, 7));
        for (auto& port : rom_ports) { port.read_enable = true; port.read_address = index; }
    }
    if (next[3].valid) {
        // OLD ROM output may fan out to a base register and the seed DSP's
        // input registers on this same edge; no newly clocked value is read.
        next[3].seed_base = p.rom[0].output() | (std::uint64_t{p.rom[1].output()} << 16) |
            (std::uint64_t{p.rom[2].output()} << 32);
        next[3].gradient = p.rom[3].output();
    }
    if (next[3].valid) ports[0] = {next[3].delta, next[3].gradient, true, false};
    if (next[6].valid) {
        if (!p.multipliers[0].output_known()) throw std::logic_error("reciprocal seed product is unknown");
        next[6].reciprocal = static_cast<std::uint64_t>(static_cast<std::int64_t>(next[6].seed_base) -
            signed_rounded(p.multipliers[0].product(), 7));
    }
    if (next[7].valid) p.wide_ports(ports, 1, next[7].normalized, 2, next[7].reciprocal, 3);
    if (next[10].valid) {
        const auto error = (std::int64_t{1} << 34) - static_cast<std::int64_t>(rounded(p.wide_result(1, 2, 3), 32));
        if (error < -131072 || error > 131071) throw std::logic_error("reciprocal Newton residual exceeds18bits");
        next[10].error = static_cast<std::int32_t>(error);
    }
    if (next[11].valid) {
        for (unsigned i = 0; i < 3; ++i)
            ports[7 + i] = {static_cast<std::int32_t>((next[11].reciprocal >> (17 * i)) & 0x1ffffu),
                next[11].error, true, false};
    }
    if (next[14].valid) {
        const auto correction = signed_rounded(p.correction_product(), 34);
        next[14].reciprocal = static_cast<std::uint64_t>(static_cast<std::int64_t>(next[14].reciprocal) + correction);
    }
    if (next[15].valid) {
        for (unsigned coordinate = 0; coordinate < 2; ++coordinate)
            p.wide_ports(ports, 10 + 9 * coordinate, next[15].magnitude[coordinate], 3, next[15].reciprocal, 3);
    }
    if (next[18].valid) {
        for (unsigned coordinate = 0; coordinate < 2; ++coordinate) {
            const auto estimate = rounded(p.wide_result(10 + 9 * coordinate, 3, 3), next[18].exponent + 8);
            if (estimate > std::numeric_limits<std::uint32_t>::max())
                throw std::logic_error("reciprocal estimate exceeds32bits");
            next[18].quotient[coordinate] = static_cast<std::uint32_t>(estimate);
        }
    }
    if (next[19].valid) {
        for (unsigned coordinate = 0; coordinate < 2; ++coordinate)
            p.wide_ports(ports, 28 + 6 * coordinate, next[19].q, 3, next[19].quotient[coordinate], 2);
    }
    if (next[22].valid) {
        for (unsigned coordinate = 0; coordinate < 2; ++coordinate)
            next[22].remainder[coordinate] = difference(shifted(next[22].magnitude[coordinate], 26),
                p.wide_result(28 + 6 * coordinate, 3, 2));
    }
    for (const auto at : {23u, 24u}) {
        if (!next[at].valid) continue;
        for (unsigned coordinate = 0; coordinate < 2; ++coordinate) {
            auto& remainder = next[at].remainder[coordinate]; auto& quotient = next[at].quotient[coordinate];
            if (remainder < 0) { --quotient; remainder += static_cast<std::int64_t>(next[at].q); }
            else if (static_cast<std::uint64_t>(remainder) >= next[at].q) {
                ++quotient; remainder -= static_cast<std::int64_t>(next[at].q);
            }
        }
    }
    Output output; output.valid = next[25].valid; output.tag = next[25].tag;
    output.overflow = next[25].overflow; output.invalid_q = next[25].invalid_q;
    if (output.valid) {
        std::array<std::int32_t, 2> coordinates{};
        for (unsigned coordinate = 0; coordinate < 2; ++coordinate) {
            const auto remainder = next[25].remainder[coordinate];
            if (remainder < 0 || static_cast<std::uint64_t>(remainder) >= next[25].q)
                throw std::logic_error("reciprocal exact correction did not establish floor quotient");
            std::uint64_t quotient = next[25].quotient[coordinate];
            const auto twice = static_cast<std::uint64_t>(remainder) << 1;
            if (twice > next[25].q || (twice == next[25].q && (quotient & 1u))) ++quotient;
            const auto limit = next[25].negative[coordinate] ? std::uint64_t{1} << 31 :
                (std::uint64_t{1} << 31) - 1;
            if (quotient > limit) output.overflow = true;
            else if (next[25].negative[coordinate] && quotient == (std::uint64_t{1} << 31))
                coordinates[coordinate] = std::numeric_limits<std::int32_t>::min();
            else coordinates[coordinate] = next[25].negative[coordinate] ?
                -static_cast<std::int32_t>(quotient) : static_cast<std::int32_t>(quotient);
        }
        output.u = coordinates[0]; output.v = coordinates[1];
    }
    for (unsigned i = 0; i < mult18_equivalents; ++i) p.multipliers[i].tick(ports[i]);
    for (unsigned i = 0; i < seed_bsram_blocks; ++i) p.rom[i].tick(rom_ports[i]);
    p.stages = next; p.output = output;
}

const PerspectivePipeline::Output& PerspectivePipeline::output() const { return impl_->output; }
bool PerspectivePipeline::busy() const {
    for (const auto& stage : impl_->stages) if (stage.valid) return true;
    return false;
}
void PerspectivePipeline::reset() {
    auto& p = *impl_; p.stages = {}; p.output = {};
    for (auto& multiplier : p.multipliers) multiplier.tick({0, 0, false, true});
    for (auto& bank : p.rom) bank.tick({false, 0, 0, false, 0, true});
}

} // namespace nano_gpu::hw
