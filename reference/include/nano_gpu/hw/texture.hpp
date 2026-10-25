#pragma once

#include "nano_gpu/reference.hpp"
#include "nano_gpu/hw/resources.hpp"

#include <cstdint>
#include <memory>

namespace nano_gpu::hw {

class SdramController;

struct TextureStatistics {
    std::uint64_t cycles = 0;
    std::uint64_t stalls = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t refill_words = 0;
    std::uint64_t memory_wait_cycles = 0;
    std::uint64_t pipeline_inputs = 0;
    std::uint64_t pipeline_outputs = 0;
    std::uint64_t pipeline_backpressure_cycles = 0;
};

// One selected Gowin SDPB 1024x16 supplies a 64-line direct-mapped cache,
// with 16 RGB565 words per line. Tags and validity are bounded registers.
// The serial oracle retains one request. The selected streaming alternative
// retains at most eight stages/reservations and can accept one hit per edge;
// cache reads/writes always use the same clocked SDPB ports.
// The local block profile uses an immutable Image backing adapter: a miss has
// four response-wait edges, then one word per edge for its 16-word refill.
// The system profile instead waits for shared SDRAM requests and streams
// response-slot synchronous reads through one 32-bit capture register into
// two 16-bit cache writes; the slot stays owned until its real words drain.
// The serial oracle uses a ten-edge shift/add address generator. The streaming
// selection uses two native18x18 multipliers and registered lookup/read stages.
class TextureUnit {
public:
    static constexpr unsigned cache_words = 1024;
    static constexpr unsigned line_words = 16;
    static constexpr unsigned line_count = 64;
    static constexpr unsigned address_edges = 10;
    static constexpr unsigned backing_response_edges = 4;
    static constexpr unsigned pipeline_address_multipliers = 2;
    static constexpr unsigned pipeline_hit_edges = 7;
    static constexpr unsigned streaming_capacity = 8;

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
    // System-model binding. Cache misses issue bounded reads through the
    // shared SDRAM Texture client and wait for its registered response. This
    // unit never advances the SDRAM clock and never reads an Image pixel.
    void bind_external(const TextureDescriptor& descriptor, SdramController& memory);
    void invalidate();

    // Explicit copies reuse this physical cache bank while rasterization is
    // idle. Writes invalidate cache tags. Each access costs one edge; the
    // result getter observes a previously established synchronous read.
    void scratch_write(std::uint16_t address, std::uint16_t value);
    void scratch_read_request(std::uint16_t address);
    std::uint16_t scratch_read_result() const;

    // Capture an already clamped/repeated integer texel coordinate in one edge.
    // Following it, a hit needs ten address edges, a tag edge, a read edge and
    // a capture edge. A miss additionally waits/refills as described above.
    // A completed old result may be replaced by a new request.
    void start(int x, int y);
    bool busy() const;
    bool external_memory_pending() const;
    bool result_valid() const;
    std::uint16_t result() const;

    // Static architectural selection, changed only while completely drained.
    // It consumes no device edge. The default retains the serial block oracle;
    // the GPU selects the streaming path, which shares this same cache bank.
    // Reset preserves the selection. Bind and scratch access still require all
    // accepted requests/results to have drained.
    void pipeline_mode(bool enabled);
    bool pipeline_mode() const;
    // push/pop reserve their handshake until tick(). At most one producer and
    // one consumer may reserve an edge. A disabled tick preserves reservations
    // and all pipeline state. The caller samples OLD output before that edge.
    bool input_ready() const;
    bool push(int x, int y);
    bool output_valid() const;
    std::uint16_t output() const;
    bool pop();
    // Includes all pipeline stages, the result register and a reserved input.
    unsigned inflight() const;

    // Apply one common rising edge. Disabled edges hold all controller/cache
    // state and count only as elapsed stalled clocks.
    void tick(bool enable = true);

    // One synchronous edge cancels a request, unbinds the descriptor, resets
    // cache output and tag/controller registers, and preserves cache RAM words.
    // Statistics restart and include this reset edge. An accepted external
    // SDRAM request must be consumed first; reset otherwise throws unchanged.
    void reset();
    const TextureStatistics& statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nano_gpu::hw
