#include "nano_gpu/hw/blit.hpp"

#include "nano_gpu/hw/bsram.hpp"

#include <stdexcept>

namespace nano_gpu::hw {
namespace {

void require_region(const CopyRegion& region, int source_width, int source_height,
                    std::size_t destination_size) {
    if (destination_size != static_cast<std::size_t>(screen_width) * screen_height ||
        region.width < 0 || region.height < 0 || region.width > screen_width ||
        region.height > screen_height || region.source.x < 0 || region.source.y < 0 ||
        region.destination.x < 0 || region.destination.y < 0 ||
        static_cast<std::int64_t>(region.source.x) + region.width > source_width ||
        static_cast<std::int64_t>(region.source.y) + region.height > source_height ||
        static_cast<std::int64_t>(region.destination.x) + region.width > screen_width ||
        static_cast<std::int64_t>(region.destination.y) + region.height > screen_height) {
        throw std::invalid_argument("hardware copy region must be clipped to both images");
    }
}

template<class ReadSource>
CopyStatistics copy_rows(ReadSource&& read_source, std::vector<std::uint16_t>& destination,
                         const CopyRegion& region, bool reverse_rows) {
    CopyStatistics counters;
    if (region.width == 0 || region.height == 0) return counters;
    Sdp1kx16 row_buffer;
    row_buffer.tick({false, 0, 0, false, 0, true});
    ++counters.cycles;
    for (int visit = 0; visit < region.height; ++visit) {
        const int row = reverse_rows ? region.height - 1 - visit : visit;
        for (int word = 0; word < region.width; ++word) {
            const auto value = read_source(region.source.x + word, region.source.y + row);
            row_buffer.tick({true, static_cast<std::uint16_t>(word), value, false, 0, false});
            ++counters.cycles;
        }
        for (int beat = 0; beat <= region.width; ++beat) {
            if (beat != 0) {
                // Functional write adapter samples the old registered B output.
                const auto index = static_cast<std::size_t>(region.destination.y + row) *
                                   screen_width + region.destination.x + beat - 1;
                destination[index] = row_buffer.output();
                ++counters.words;
            }
            row_buffer.tick({false, 0, 0, beat < region.width,
                             static_cast<std::uint16_t>(beat < region.width ? beat : 0), false});
            ++counters.cycles;
        }
    }
    return counters;
}

} // namespace

CopyStatistics copy_framebuffer(const std::vector<std::uint16_t>& source,
                                std::vector<std::uint16_t>& destination,
                                const CopyRegion& region) {
    require_region(region, screen_width, screen_height, destination.size());
    if (source.size() != destination.size())
        throw std::invalid_argument("hardware framebuffer copy requires 640x480 source");
    const bool reverse = &source == &destination && region.destination.y > region.source.y;
    return copy_rows([&](int x, int y) {
        return source[static_cast<std::size_t>(y) * screen_width + x];
    }, destination, region, reverse);
}

CopyStatistics copy_from_image(const Image& source, std::vector<std::uint16_t>& destination,
                               const CopyRegion& region) {
    require_region(region, source.width(), source.height(), destination.size());
    return copy_rows([&](int x, int y) { return source.at(x, y); }, destination, region, false);
}

} // namespace nano_gpu::hw
