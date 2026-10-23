#pragma once

#include "nano_gpu/reference.hpp"
#include "nano_gpu/hw/resources.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace nano_gpu::hw {

class SdramController;

struct TileWord {
    std::uint16_t color = 0;
    std::uint32_t depth = 0;
};

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
    std::uint64_t keyed_rejected = 0;
    std::uint64_t texture_samples = 0;
    std::uint64_t color_writes = 0;
    std::uint64_t depth_writes = 0;
    std::uint64_t forwarded_reads = 0;
    std::uint64_t load_words = 0;
    std::uint64_t store_words = 0;
    std::uint64_t perspective_cycles = 0;
    std::uint64_t texture_cycles = 0;
    std::uint64_t texture_hits = 0;
    std::uint64_t texture_misses = 0;
    std::uint64_t texture_refill_words = 0;
};

// Rendering slice: three SDPB 1024x16 tile banks, one SDPB texture cache,
// one shared signed MULT18X18, and one explicit restoring divider. The original
// class name is retained for source compatibility. No canonical raster fallback
// is used for triangles, rectangles, lines, or keyed sprites.
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
    // An outstanding external texture SDRAM response must first be drained;
    // attempting reset while it is owned throws without changing tile state.
    void reset();

    // Blocking adapters for a ready tile. Loading/clearing costs 1024 rising
    // edges. Store uses 1024 read requests and one output-drain edge. These
    // counts describe local BSRAM ports, not SDRAM transaction timing.
    void load(const TileImage& image);
    void clear(std::uint16_t color, std::uint32_t depth);
    TileImage store();

    // System DMA ports: exactly one common edge per call, ready tile only.
    // read_word_result() observes a previously issued synchronous read and
    // costs no edge; the caller captures it before advancing the next edge.
    void load_word(std::uint16_t address, std::uint16_t color, std::uint32_t depth);
    void read_word_request(std::uint16_t address);
    TileWord read_word_result() const;
    void idle_edge();
    void invalidate_texture();
    void copy_buffer_write(std::uint16_t address, std::uint16_t value);
    void copy_buffer_read_request(std::uint16_t address);
    std::uint16_t copy_buffer_read_result() const;

    // begin latches one prepared command on one edge. The tile origin must be
    // a visible 32x32 tile boundary. CPU preparation/validation is external.
    void begin(const Triangle& triangle, Point tile_origin);
    void begin(const Primitive& primitive, Point tile_origin);
    void begin_external(const Primitive& primitive, Point tile_origin,
                        const std::optional<TextureDescriptor>& texture,
                        SdramController& memory);
    bool busy() const;
    // Recovery may keep clocking until issued texture memory work retires,
    // then reset/cancel before the retained fragment commits.
    bool external_memory_pending() const;

    // One common rising edge. enable=false freezes every datapath/memory stage
    // and counts a stalled clock. Setup and covered-fragment traffic are ordered.
    void tick(bool enable = true);
    const TileStatistics& statistics() const;

private:
    void begin_impl(const Primitive& primitive, Point tile_origin,
                    const std::optional<TextureDescriptor>& texture,
                    SdramController* memory);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Functional host adapter: CPU bins and idealized tile input/output feed the
// clocked slice. Record fetching, SDRAM arbitration, and bus timing are not
// represented by this adapter. Unsupported records fail before attachment writes.
void render_pass(const Pass& pass, std::vector<std::uint16_t>& color,
                 std::vector<std::uint32_t>& depth, Statistics& statistics);

// Compatibility entry point; now executes the complete rendering profile.
void render_opaque_pass(const Pass& pass, std::vector<std::uint16_t>& color,
                        std::vector<std::uint32_t>& depth, Statistics& statistics);

} // namespace nano_gpu::hw
