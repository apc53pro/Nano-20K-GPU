#pragma once

#include "nano_gpu/reference.hpp"
#include "nano_gpu/hw/bsram.hpp"
#include "nano_gpu/hw/resources.hpp"

#include <array>
#include <cstdint>
#include <functional>
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

// Host-only observations of the registered datapath. IDs, callbacks and the
// last-event snapshot are instrumentation, not extra GPU registers or ports.
// A covered sample is accepted exactly once; it retires exactly once by commit,
// depth/key rejection, or cancellation on reset. Color and depth writes belong
// to one retirement and must not be counted as two rendered fragments.
enum class FragmentStage {
    Idle, Transfer, Setup, Raster, Depth, PerspectiveU, PerspectiveV,
    Texture, Shade, Commit, Drain, Reset
};
enum class FragmentRetirement { None, Commit, DepthRejected, KeyRejected, Cancelled };
inline constexpr unsigned fragment_trace_capacity = 64;

const char* fragment_stage_name(FragmentStage stage);
const char* fragment_retirement_name(FragmentRetirement retirement);

struct FragmentToken {
    // Nonzero IDs are monotonically assigned across resets of this tile object.
    std::uint64_t id = 0;
    std::uint64_t primitive_id = 0;
    int x = 0, y = 0;
    std::uint16_t address = 0;
    bool textured = false;
};

struct FragmentCycle {
    // Existing tile-local elapsed-edge count; reset restarts it at one. The
    // system scheduler supplies its own common-clock timestamp to observers.
    std::uint64_t cycle = 0;
    FragmentStage stage = FragmentStage::Idle;
    bool enabled = true;
    bool candidate = false, covered = false;
    bool accepted = false, retired = false;
    FragmentToken accepted_fragment{}, retired_fragment{};
    FragmentRetirement retirement = FragmentRetirement::None;
    bool color_write = false, depth_write = false;
    unsigned inflight_before = 0, inflight_after = 0;
    // A reset may atomically discard a whole bounded pipeline. Batch
    // cancellation is exclusive with normal acceptance/retirement/write flags.
    // The emitter owns this host-only snapshot; its storage remains valid
    // through callbacks and last_cycle() observation until the next local edge.
    const FragmentToken* cancelled_fragments = nullptr;
    unsigned cancelled_count = 0;
};

using FragmentCycleObserver = std::function<void(const FragmentCycle&)>;

// Rendering slice: three native tile banks, one texture cache, six fragment
// context banks and four reciprocal seed-ROM banks. The selected pipeline uses
// 46 native multiplier equivalents and exact corrected UV reconstruction.
// The original class name is retained for source compatibility; primitives do
// not fall back to canonical software rasterization.
class OpaqueTile {
public:
    using AttachmentBanks = std::array<Sdp1kx16, 3>;
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
    // One role-select control edge, ready tile only. Both sets retain their
    // fixed physical RAMs; only the selected bank roles change. No contents
    // move and no RAM ports are accessed. The caller's bank set must remain
    // alive and connected until this tile is destroyed, with no staged port
    // operation on this edge. Results still require a later synchronous read.
    void exchange_attachment_banks(AttachmentBanks*& idle_banks);
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
    // Recovery-only edge: drain an issued texture memory operation while
    // freezing fragment processing and attachment commits. Reset must follow.
    void drain_external_memory_edge();

    // One common rising edge. enable=false freezes every datapath/memory stage
    // and counts a stalled clock. Setup and covered-fragment traffic are ordered.
    void tick(bool enable = true);
    const TileStatistics& statistics() const;
    // The accessor is replaced on every local edge, including idle, transfer,
    // command capture, reset and disabled edges; stale accept/retire flags are
    // never replayed. The optional observer receives the same completed event
    // exactly once per edge. It must not mutate/re-enter this tile.
    const FragmentCycle& last_cycle() const;
    void set_cycle_observer(FragmentCycleObserver observer);

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
