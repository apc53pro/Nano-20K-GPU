#pragma once

#include "nano_gpu/hw/blit.hpp"
#include "nano_gpu/hw/protocol.hpp"

#include <memory>

namespace nano_gpu::hw {

class OpaqueTile;
class SdramController;

// Clocked system copy engine. A complete clipped row occupies the tile's
// existing texture-cache BSRAM; no additional row RAM is allocated. Source
// reads and masked destination writes stream through the shared SDRAM Copy
// client's native synchronous slot ports. Only one 32-bit read hold and one
// 32-bit write assembly register are retained outside the existing row bank.
// Front/back ownership and attachment validity are checked by the executor.
class SystemCopy {
public:
    SystemCopy();
    ~SystemCopy();
    SystemCopy(const SystemCopy&) = delete;
    SystemCopy& operator=(const SystemCopy&) = delete;
    SystemCopy(SystemCopy&&) noexcept;
    SystemCopy& operator=(SystemCopy&&) noexcept;

    // One command-capture edge. All structural/range/clip checks precede
    // mutation. Fully clipped commands complete without touching memory.
    void begin(const CopyCommand& command);
    // One common edge. The global owner clocks SDRAM separately. Disabled
    // edges hold engine/BSRAM state while responses remain bounded in SDRAM.
    void tick(SdramController& memory, OpaqueTile& tile, bool enable = true);
    bool busy() const;
    // Remains true until every required word of an owned response is drained.
    bool memory_pending() const;
    // Recovery may cancel once its accepted memory response has retired.
    // Unsubmitted words in the Copy slot are discarded before resetting.
    void reset();
    const CopyStatistics& statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nano_gpu::hw
