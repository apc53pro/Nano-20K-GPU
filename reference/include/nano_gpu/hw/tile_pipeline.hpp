#pragma once

#include "nano_gpu/reference.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace nano_gpu::hw {

struct TileImage {
    std::array<std::uint16_t, tile_size * tile_size> color{};
    std::array<std::uint32_t, tile_size * tile_size> depth{};
};

struct TileStatistics {
    std::uint64_t cycles = 0;
    std::uint64_t stalls = 0;
    std::uint64_t setup_cycles = 0;
    std::uint64_t candidates = 0;
    std::uint64_t covered_fragments = 0;
    std::uint64_t depth_rejected = 0;
    std::uint64_t color_writes = 0;
    std::uint64_t depth_writes = 0;
    std::uint64_t forwarded_reads = 0;
    std::uint64_t load_words = 0;
    std::uint64_t store_words = 0;
};

// First opaque raster slice: three selected SDPB 1024x16 banks (color, depth
// low16, depth high8) and one shared signed MULT18X18. Flat/Gouraud triangles
// only. Every memory access goes through a clocked primitive port.
class OpaqueTile {
public:
    OpaqueTile();
    ~OpaqueTile();
    OpaqueTile(const OpaqueTile&) = delete;
    OpaqueTile& operator=(const OpaqueTile&) = delete;
    OpaqueTile(OpaqueTile&&) noexcept;
    OpaqueTile& operator=(OpaqueTile&&) noexcept;

    // One synchronous reset edge cancels pending drawing and resets outputs,
    // but preserves RAM contents. Counters restart and include the reset edge.
    void reset();

    // Blocking adapters for a ready tile. Loading/clearing costs 1024 rising
    // edges. Store uses 1024 read requests and one output-drain edge. These
    // counts describe local BSRAM ports, not SDRAM transaction timing.
    void load(const TileImage& image);
    void clear(std::uint16_t color, std::uint32_t depth);
    TileImage store();

    // begin latches one prepared command on one edge. The tile origin must be
    // a visible 32x32 tile boundary. CPU preparation/validation is external.
    void begin(const Triangle& triangle, Point tile_origin);
    bool busy() const;

    // One common rising edge. enable=false freezes every datapath/memory stage
    // and counts a stalled clock. Setup and covered-fragment traffic are ordered.
    void tick(bool enable = true);
    const TileStatistics& statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Functional host adapter: CPU bins and idealized tile input/output feed the
// clocked slice. Record fetching, SDRAM arbitration, and bus timing are not
// represented by this adapter. Unsupported records fail before attachment writes.
void render_opaque_pass(const Pass& pass, std::vector<std::uint16_t>& color,
                        std::vector<std::uint32_t>& depth, Statistics& statistics);

} // namespace nano_gpu::hw
