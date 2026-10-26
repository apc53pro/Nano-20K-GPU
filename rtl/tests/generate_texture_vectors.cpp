#include "../../reference/tests/uv_oracle.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <string>

namespace {
using nano_gpu::tests::OracleWide;
// Common denominator2^66 for the conservative relative seed-error bound in
// nano_uv_texture_pipeline. Even the largest accepted magnitude is within a
// quarter integer texel, so its floor estimate is at mostone quotient away.
constexpr OracleWide center_error = (OracleWide{1} << 24) + 1;
constexpr OracleWide relative_numerator = center_error * center_error + (OracleWide{1032} << 32);
static_assert(32769 * relative_numerator < (OracleWide{1} << 64), "coarse seed needs more correction steps");
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    std::ifstream base(argv[1]);
    std::ofstream out(argv[2]);
    if (!base || !out) return 2;
    std::size_t count = 0;
    std::string line;
    while (std::getline(base, line)) { out << line << '\n'; ++count; }
    const auto base_count = count;
    constexpr std::int64_t minimum_q = std::int64_t{1} << 29;
    constexpr std::int64_t maximum_q = std::int64_t{1} << 37;
    std::set<std::int64_t> divisors;
    const auto add_q = [&](std::int64_t q) { if (q >= minimum_q && q <= maximum_q) divisors.insert(q); };
    for (unsigned e = 29; e <= 37; ++e)
        for (const int delta : {-2, -1, 0, 1, 2}) add_q((std::int64_t{1} << e) + delta);
    for (unsigned e = 29; e < 37; ++e) {
        const auto power = std::int64_t{1} << e;
        for (const unsigned segment : {0u, 1u, 63u, 127u, 128u, 254u, 255u})
            for (const auto at : {power + (power / 256) * segment,
                                  power + (power / 512) * (2 * segment + 1)})
                for (const int delta : {-1, 0, 1}) add_q(at + delta);
    }
    std::mt19937_64 random(0x545850495045ULL);
    for (unsigned i = 0; i < 128; ++i) add_q(minimum_q + random() % (maximum_q - minimum_q + 1));
    const auto emit = [&](std::int64_t u, std::int64_t v, std::int64_t q) {
        bool overflow = false;
        std::int32_t expected_u = 0, expected_v = 0;
        try { expected_u = nano_gpu::tests::rational_uv(u, q); }
        catch (const std::overflow_error&) { overflow = true; }
        try { expected_v = nano_gpu::tests::rational_uv(v, q); }
        catch (const std::overflow_error&) { overflow = true; }
        out << std::hex << static_cast<std::uint64_t>(u) << ' ' << static_cast<std::uint64_t>(v)
            << ' ' << static_cast<std::uint64_t>(q) << ' ' << static_cast<std::uint32_t>(expected_u)
            << ' ' << static_cast<std::uint32_t>(expected_v) << ' ' << overflow << " 0\n";
        ++count;
    };
    for (const auto q : divisors) {
        for (const std::int64_t k : {0, 1, 257, 1024, 32767, 32768}) {
            // Values on either side of the signed full-Q16 half-step near an
            // integer texel. Expected values still use independent wide exact
            // division and RNE, never the DUT's seed or integer-threshold rule.
            const auto positive = ((static_cast<OracleWide>(k + 1) * q) << 17) - q;
            const auto negative = ((static_cast<OracleWide>(k) * q) << 17) + q;
            for (const auto numerator : {positive, negative}) {
                const auto center = static_cast<std::int64_t>(numerator / (OracleWide{1} << 27));
                for (const int delta : {-2, -1, 0, 1, 2}) {
                    const auto attribute = center + delta;
                    emit(attribute, 0, q); emit(-attribute, 0, q);
                    emit(0, attribute, q); emit(0, -attribute, q);
                }
            }
        }
    }
    std::cout << count << " exact texture-coordinate vectors; " << count - base_count
              << " targeted integer/RNE/overflow boundaries; analytic seed error <1/4 texel\n";
    return out ? 0 : 1;
}
