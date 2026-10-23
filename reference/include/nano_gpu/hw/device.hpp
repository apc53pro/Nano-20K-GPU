#pragma once

#include <cstdint>
#include <stdexcept>

namespace nano_gpu::hw {

// Tang Nano 20K's QN88 device, not the maximum configuration of the family.
// Sipeed board/device-selection documentation; DS226-2.7E, tables 1-1/1-2.
// Source revisions and the distinction between device facts and our selected
// primitive configurations are recorded in docs/hardware/gowin-profile.md.
struct TangNano20K {
    static constexpr const char* part = "GW2AR-LV18QN88C8/I7";
    static constexpr const char* architecture = "GW2AR-18C";
    static constexpr std::uint32_t lut4 = 20736;
    static constexpr std::uint32_t flip_flops = 15552;
    static constexpr std::uint32_t bsram_blocks = 46;
    static constexpr std::uint32_t bsram_bits_per_block = 18432;
    static constexpr std::uint32_t shadow_sram_bits = 41472;
    static constexpr std::uint32_t mult18_equivalents = 48;
    static constexpr std::uint32_t dsp_macros = 24;
    static constexpr std::uint32_t dsp_blocks = 12;
    static constexpr std::uint32_t available_plls = 2;
    static constexpr std::uint32_t sdram_bytes = 8 * 1024 * 1024;
    static constexpr std::uint32_t sdram_data_bits = 32;
};

// These are selected GPU configurations, not additional capabilities of Gowin
// primitives. Each SDPB 1024x16 instance occupies one physical 18-Kbit BSRAM.
struct OpaqueSliceResources {
    static constexpr const char* profile = "N20K-HW-OPAQUE-0.1";
    static constexpr std::uint32_t resident_tiles = 1;
    static constexpr std::uint32_t color_banks = 1;
    static constexpr std::uint32_t depth_banks = 2; // low 16 / high 8 in 16-bit RAM
    static constexpr std::uint32_t bsram_blocks = color_banks + depth_banks;
    static constexpr std::uint32_t mult18_equivalents = 1;
    static constexpr std::uint32_t edge_bits = 32;
    static constexpr std::uint32_t attribute_bits = 64;
};

// Capacity bounds only: a passing check does not establish DSP packing,
// LUT/FF use, routing, clock feasibility, or room for the rest of the GPU.
inline void check_resource_capacity(std::uint32_t bsram, std::uint32_t mult18) {
    if (bsram > TangNano20K::bsram_blocks || mult18 > TangNano20K::mult18_equivalents)
        throw std::out_of_range("hardware model exceeds Tang Nano 20K primitive capacity");
}

static_assert(TangNano20K::dsp_blocks * 2 == TangNano20K::dsp_macros);
static_assert(TangNano20K::dsp_macros * 2 == TangNano20K::mult18_equivalents);
static_assert(OpaqueSliceResources::bsram_blocks <= TangNano20K::bsram_blocks);
static_assert(OpaqueSliceResources::mult18_equivalents <= TangNano20K::mult18_equivalents);

} // namespace nano_gpu::hw
