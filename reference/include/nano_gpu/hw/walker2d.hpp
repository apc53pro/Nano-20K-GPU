#pragma once

#include "nano_gpu/reference.hpp"

#include <cstdint>

namespace nano_gpu::hw {

struct Sample2D {
    int x = 0;
    int y = 0;
    bool covered = false;
    int source_x = 0;
    int source_y = 0;
};

// Incremental Rect/Line/Sprite sample generator. It has no framebuffer port,
// pixel vector, shading logic, or unbounded queue. begin() is command capture;
// the caller counts its edge. tick() advances one candidate on an enabled edge.
//
// Grid clipping/bounds use signed 32-bit command coordinates. Visible counters
// fit x=10/y=9 bits and sprite source counters fit 10 bits. Line differences,
// error, and twice-error fit signed 16 bits for viewport-clipped endpoints.
// A line walks its original normalized endpoints for each tile; its valid flag
// filters samples after stepping, preserving Bresenham's global error history.
class Walker2D {
public:
    // Validate completely before changing the active command. Triangle records
    // belong to the coverage/attribute datapath and are rejected here.
    void begin(const Primitive& primitive, Point tile_origin);
    bool busy() const { return active_; }

    // When idle this returns covered=false. For a line, covered=false can also
    // mark a real clocked candidate outside this tile/scissor; controllers must
    // keep stepping but must not count that point as a rendered candidate.
    Sample2D sample() const;
    void tick(bool enable = true);

private:
    enum class Kind { Grid, Sprite, Line };
    Kind kind_ = Kind::Grid;
    bool active_ = false;
    Bounds window_{};
    std::int32_t x_ = 0;
    std::int32_t y_ = 0;
    std::int32_t end_x_ = 0;
    std::int32_t end_y_ = 0;
    std::int16_t dx_ = 0;
    std::int16_t dy_ = 0;
    std::int16_t sx_ = 0;
    std::int16_t sy_ = 0;
    std::int16_t error_ = 0;
    std::int32_t source_start_x_ = 0;
    std::int32_t source_x_ = 0;
    std::int32_t source_y_ = 0;
};

} // namespace nano_gpu::hw
