#pragma once

#include "nano_gpu/reference.hpp"

#include <cstdint>
#include <memory>

namespace nano_gpu::hw {

struct TextureStatistics {
    std::uint64_t cycles = 0;
    std::uint64_t stalls = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t refill_words = 0;
};

// One selected Gowin SDPB 1024x16 supplies a 64-line direct-mapped cache,
// with 16 RGB565 words per line. Tags and validity are bounded registers.
// A single request is outstanding; cache reads/writes use clocked SDPB ports.
// The backing Image is an immutable, idealized memory-response adapter, not
// a timed SDRAM controller. A miss has four response-wait edges, then one word
// per edge for its 16-word refill. Addresses use a ten-edge shift/add generator.
class TextureUnit {
public:
    static constexpr unsigned cache_words = 1024;
    static constexpr unsigned line_words = 16;
    static constexpr unsigned line_count = 64;
    static constexpr unsigned address_edges = 10;
    static constexpr unsigned backing_response_edges = 4;

    TextureUnit();
    ~TextureUnit();
    TextureUnit(const TextureUnit&) = delete;
    TextureUnit& operator=(const TextureUnit&) = delete;
    TextureUnit(TextureUnit&&) noexcept;
    TextureUnit& operator=(TextureUnit&&) noexcept;

    // bind and invalidate each consume one control edge, require an idle unit,
    // and clear result_valid. A changed binding invalidates all line tags;
    // rebinding the same immutable Image retains cache contents.
    void bind(std::shared_ptr<const Image> texture);
    void invalidate();

    // Capture an already clamped/repeated integer texel coordinate in one edge.
    // Following it, a hit needs ten address edges, a tag edge, a read edge and
    // a capture edge. A miss additionally waits/refills as described above.
    // A completed old result may be replaced by a new request.
    void start(int x, int y);
    bool busy() const;
    bool result_valid() const;
    std::uint16_t result() const;

    // Apply one common rising edge. Disabled edges hold all controller/cache
    // state and count only as elapsed stalled clocks.
    void tick(bool enable = true);

    // One synchronous edge cancels a request, unbinds the descriptor, resets
    // cache output and tag/controller registers, and preserves cache RAM words.
    // Statistics restart and include this reset edge.
    void reset();
    const TextureStatistics& statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nano_gpu::hw
