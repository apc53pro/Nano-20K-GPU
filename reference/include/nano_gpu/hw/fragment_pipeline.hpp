#pragma once

#include "nano_gpu/hw/tile_pipeline.hpp"

#include <cstdint>
#include <memory>

namespace nano_gpu::hw {

class TextureUnit;

// Clocked textured-fragment datapath. Context payloads reside in six selected
// SDPB 1024x16 banks; the64-entry in-order ROB retains only ready/reason flags.
// The enclosing tile owns D24 ports and observer identities. This block owns
// the shared texture unit's clock whenever its textured pipeline is active.
class TexturedFragmentPipeline {
public:
    static constexpr unsigned capacity = 64;
    static constexpr unsigned context_bsram_blocks = 6;
    static constexpr unsigned shade_mult18_equivalents = 3;
    static constexpr unsigned factor_read_edges = 1;
    static constexpr unsigned factor_capture_edges = 1;
    static constexpr unsigned shade_enabled_edges = 4;
    static constexpr unsigned retirement_read_edges = 1;
    static constexpr unsigned retirement_capture_edges = 1;

    struct Input {
        bool valid = false, sprite = false;
        std::uint16_t address = 0;
        std::uint32_t depth = 0;
        Color factor{};
        std::int64_t u = 0, v = 0, q = std::int64_t{1} << 29;
        int source_x = 0, source_y = 0;
        bool forwarded = false;
        std::uint32_t forwarded_depth = 0;
    };
    struct Result {
        bool accepted = false;
        std::uint8_t accepted_slot = 0;
        bool retired = false;
        std::uint8_t retired_slot = 0;
        FragmentRetirement reason = FragmentRetirement::None;
        std::uint16_t color = 0;
        std::uint32_t depth = 0;
        std::uint16_t address = 0;
        bool color_write = false, depth_write = false;
    };

    explicit TexturedFragmentPipeline(TextureUnit& texture);
    ~TexturedFragmentPipeline();
    TexturedFragmentPipeline(const TexturedFragmentPipeline&) = delete;
    TexturedFragmentPipeline& operator=(const TexturedFragmentPipeline&) = delete;
    TexturedFragmentPipeline(TexturedFragmentPipeline&&) noexcept;
    TexturedFragmentPipeline& operator=(TexturedFragmentPipeline&&) noexcept;

    // Static per-primitive state; bind the shared texture before configuration.
    // Configuration consumes no extra edge and requires every old context and
    // unit result to have drained. Shader inputs use raw replacement for
    // sprites/TextureReplace, and RGB modulation for the other texture modes.
    void configure(const RenderState& state);

    // One common edge. stored_depth is the OLD synchronous D24 result for the
    // input accepted on the preceding edge. Issue a new D24 read only when the
    // returned accepted flag is true. Result writes are independent and retire
    // in acceptance order. CE-low freezes all units and returns no events.
    Result tick(const Input& input, std::uint32_t stored_depth, bool enable = true);
    // Conservative ready hint; tick can also consume an old depth stage and
    // accept its successor on the same edge. Use Result::accepted as handshake.
    bool input_ready() const;
    bool busy() const;
    unsigned inflight() const;
    bool perspective_busy() const;
    bool external_memory_pending() const;

    // One edge for abort recovery: advance only the texture controller's
    // issued external memory transaction, preserving every fragment context
    // and withholding all attachment retirements until the subsequent reset.
    void drain_external();
    // One synchronous edge resets context outputs, UV/shader units and shared
    // texture state, preserving native RAM contents. External requests must
    // first drain; root-level observer logic cancels all retained token IDs.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nano_gpu::hw
