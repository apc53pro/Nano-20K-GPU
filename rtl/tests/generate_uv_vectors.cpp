#include "../../reference/tests/uv_oracle.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    auto vectors = nano_gpu::tests::perspective_vectors();
    const auto full_range = nano_gpu::tests::full_q16_vectors();
    vectors.insert(vectors.end(), full_range.begin(), full_range.end());
    for (const std::int64_t q : {std::int64_t{1} << 29, std::int64_t{1} << 37}) {
        const auto scale = q >> 26;
        for (const std::int64_t at : {std::int64_t{0}, std::int64_t{1}, std::int64_t{2},
                std::int64_t{0x7fffffff}, std::int64_t{0x80000000}})
            for (const int offset : {-1, 0, 1})
                vectors.push_back({at * scale + offset, -at * scale - offset, q});
        vectors.push_back({std::numeric_limits<std::int64_t>::min(), 0, q});
        vectors.push_back({0, std::numeric_limits<std::int64_t>::max(), q});
    }
    vectors.push_back({0, 0, (std::int64_t{1} << 29) - 1});
    vectors.push_back({0, 0, (std::int64_t{1} << 37) + 1});
    vectors.push_back({0, 0, 0});
    vectors.push_back({0, 0, -1});
    std::ofstream out(argv[1]);
    if (!out) return 2;
    for (const auto& vector : vectors) {
        const bool invalid_q = vector.q < (std::int64_t{1} << 29) || vector.q > (std::int64_t{1} << 37);
        bool overflow = false;
        std::int32_t u = 0, v = 0;
        if (!invalid_q) {
            try { u = nano_gpu::tests::rational_uv(vector.u, vector.q); }
            catch (const std::overflow_error&) { overflow = true; }
            try { v = nano_gpu::tests::rational_uv(vector.v, vector.q); }
            catch (const std::overflow_error&) { overflow = true; }
        }
        out << std::hex << static_cast<std::uint64_t>(vector.u) << ' '
            << static_cast<std::uint64_t>(vector.v) << ' ' << static_cast<std::uint64_t>(vector.q) << ' '
            << static_cast<std::uint32_t>(u) << ' ' << static_cast<std::uint32_t>(v) << ' '
            << overflow << ' ' << invalid_q << '\n';
    }
    std::cout << vectors.size() << " exact rational RTL vectors generated\n";
    return out ? 0 : 1;
}
