#pragma once

#include "nano_gpu/reference.hpp"

#include <cstdint>
#include <vector>

namespace nano_gpu::hw {

// Already clipped, nonnegative coordinates at the functional memory adapter.
struct CopyRegion {
    Point source;
    Point destination;
    int width = 0;
    int height = 0;
};

struct CopyStatistics {
    std::uint64_t cycles = 0;
    std::uint64_t words = 0;
};

// One SDPB 1024x16 row buffer, sharing the texture-cache allocation while draws
// are idle. External reads/writes are functional memory responses, not SDRAM
// service timing. Each row costs width writes + width reads + one drain edge;
// a synchronous output reset precedes the transfer. Overlap uses row ordering.
CopyStatistics copy_framebuffer(const std::vector<std::uint16_t>& source,
                                std::vector<std::uint16_t>& destination,
                                const CopyRegion& region);
CopyStatistics copy_from_image(const Image& source,
                               std::vector<std::uint16_t>& destination,
                               const CopyRegion& region);

} // namespace nano_gpu::hw
