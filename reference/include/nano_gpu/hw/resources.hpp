#pragma once

#include <cstdint>

namespace nano_gpu::hw {

// M0 physical SDRAM layout. Byte addresses, four-byte aligned.
constexpr std::uint32_t color_buffer_bytes = 640u * 480u * 2u;
constexpr std::uint32_t color_buffer_base(unsigned id) { return id * color_buffer_bytes; }
constexpr std::uint32_t depth_buffer_base = 0x12c000u;
constexpr std::uint32_t depth_buffer_bytes = 640u * 480u * 4u;
constexpr std::uint32_t heap_base = 0x258000u;
constexpr std::uint32_t memory_bytes = 0x800000u;

// A fetched descriptor, not a host pixel pointer. Storage includes row padding.
struct TextureDescriptor {
    std::uint32_t base = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch_bytes = 0;
};
inline bool operator==(const TextureDescriptor& a, const TextureDescriptor& b) {
    return a.base == b.base && a.width == b.width && a.height == b.height &&
           a.pitch_bytes == b.pitch_bytes;
}
inline bool operator!=(const TextureDescriptor& a, const TextureDescriptor& b) { return !(a == b); }

} // namespace nano_gpu::hw
