#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace nano_gpu::tests {

// Test-only exact rational arithmetic. The DUT must not use this host-wide
// division or share an approximation with this independently calculated oracle.
using OracleWide = __int128;

inline std::int32_t rational_uv(std::int64_t attribute, std::int64_t q) {
    if (q <= 0) throw std::invalid_argument("oracle requires positive q");
    const OracleWide signed_numerator = static_cast<OracleWide>(attribute) * (OracleWide{1} << 26);
    const bool negative = signed_numerator < 0;
    const auto numerator = negative ? -signed_numerator : signed_numerator;
    auto quotient = numerator / q;
    const auto remainder = numerator % q;
    if (2 * remainder > q || (2 * remainder == q && (quotient & 1))) ++quotient;
    const auto result = negative ? -quotient : quotient;
    if (result < std::numeric_limits<std::int32_t>::min() ||
        result > std::numeric_limits<std::int32_t>::max())
        throw std::overflow_error("oracle UV exceeds signed Q16");
    return static_cast<std::int32_t>(result);
}

struct UvVector {
    std::int64_t u = 0, v = 0, q = std::int64_t{1} << 29;
};

// q powers, normalization/ROM-bin boundaries and texel boundaries are selected
// independently of any reciprocal coefficients or implementation lookup table.
// All ordinary vectors lie within the M0 +/-1024-texel input domain. Broader
// signed-Q16 endpoints can be tested separately when the selected unit supports
// them; they are not needed to justify narrowing within the M0 contract.
inline std::vector<UvVector> perspective_vectors() {
    constexpr std::int64_t minimum_q = std::int64_t{1} << 29;
    constexpr std::int64_t maximum_q = std::int64_t{1} << 37;
    std::vector<UvVector> result;
    const auto add = [&](std::int64_t u, std::int64_t v, std::int64_t q) {
        if (q >= minimum_q && q <= maximum_q) result.push_back({u, v, q});
    };
    const auto boundaries = [&](std::int64_t q) {
        if (q < minimum_q || q > maximum_q) return;
        for (const auto texel : {-1024, -257, -1, 0, 1, 255, 1024}) {
            const auto numerator = static_cast<OracleWide>(texel) * q;
            const auto at = static_cast<std::int64_t>(numerator / 1024);
            for (const auto offset : {-3, -1, 0, 1, 3})
                add(at + offset, -at - offset, q);
        }
    };
    for (unsigned exponent = 29; exponent <= 37; ++exponent) {
        const auto power = std::int64_t{1} << exponent;
        boundaries(power - 1); boundaries(power); boundaries(power + 1);
        const auto scale = std::int64_t{1} << (exponent - 26);
        // Exact positive/negative halfway cases with even and odd quotients.
        for (const auto quotient : {0, 1, 2, 3, 65535, 65536, 67108862, 67108863}) {
            const auto tied = std::int64_t{quotient} * scale + scale / 2;
            add(tied, -tied, power);
            add(tied - 1, tied + 1, power);
        }
        if (exponent == 37) continue;
        for (unsigned bin = 0; bin < 1024; ++bin) {
            const auto q = power + (std::int64_t{bin} << (exponent - 10));
            for (const auto offset : {-1, 0, 1}) {
                const auto sample_q = q + offset;
                if (sample_q < minimum_q || sample_q > maximum_q) continue;
                // Two distant coordinate magnitudes and signs share q.
                const auto a = static_cast<std::int64_t>((OracleWide{1023} * sample_q) / 1024);
                const auto b = static_cast<std::int64_t>((OracleWide{-17} * sample_q) / 1024);
                add(a + (bin % 3) - 1, b - (bin % 5) + 2, sample_q);
            }
        }
    }
    std::mt19937_64 random(0x5556504950454c49ULL);
    for (unsigned i = 0; i < 8192; ++i) {
        const auto q = minimum_q + static_cast<std::int64_t>(random() % (maximum_q - minimum_q + 1));
        const auto a = static_cast<std::int64_t>(random() % 134217729u) - 67108864;
        const auto b = static_cast<std::int64_t>(random() % 134217729u) - 67108864;
        const auto u = static_cast<std::int64_t>((static_cast<OracleWide>(a) * q) / (OracleWide{1} << 26));
        const auto v = static_cast<std::int64_t>((static_cast<OracleWide>(b) * q) / (OracleWide{1} << 26));
        add(u, v, q);
    }
    return result;
}

inline std::vector<UvVector> full_q16_vectors() {
    constexpr auto minimum_q = std::int64_t{1} << 29;
    constexpr auto maximum_q = std::int64_t{1} << 37;
    std::vector<UvVector> result;
    std::mt19937_64 random(0x524f554e44434f52ULL);
    for (unsigned i = 0; i < 32768; ++i) {
        const auto q = minimum_q + static_cast<std::int64_t>(random() % (maximum_q - minimum_q + 1));
        const auto ua = static_cast<std::int64_t>(random() & 0xffffffffu) - (std::int64_t{1} << 31);
        const auto va = static_cast<std::int64_t>(random() & 0xffffffffu) - (std::int64_t{1} << 31);
        const auto u = static_cast<std::int64_t>((static_cast<OracleWide>(ua) * q) / (OracleWide{1} << 26));
        const auto v = static_cast<std::int64_t>((static_cast<OracleWide>(va) * q) / (OracleWide{1} << 26));
        result.push_back({u + static_cast<std::int64_t>(i % 7) - 3,
                          v - static_cast<std::int64_t>(i % 11) + 5, q});
    }
    // All discarded low-bit patterns close to a normalization carry, at large
    // positive and negative coordinate magnitudes. Some endpoint perturbations
    // deliberately overflow and must be emitted as flagged results.
    for (unsigned exponent = 33; exponent <= 37; ++exponent) {
        for (int offset = -64; offset <= 1; ++offset) {
            const auto q = (std::int64_t{1} << exponent) + offset;
            if (q < minimum_q || q > maximum_q) continue;
            const auto magnitude = static_cast<std::int64_t>(
                (static_cast<OracleWide>(std::numeric_limits<std::int32_t>::max()) * q) /
                (OracleWide{1} << 26));
            for (const auto delta : {-8, -1, 0, 1, 8})
                result.push_back({magnitude + delta, -magnitude - delta, q});
        }
    }
    return result;
}

} // namespace nano_gpu::tests
