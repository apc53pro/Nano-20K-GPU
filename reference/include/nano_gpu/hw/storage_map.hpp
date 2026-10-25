#pragma once

#include "nano_gpu/hw/device.hpp"

#include <array>
#include <cstdint>

namespace nano_gpu::hw {

// Selected physical storage, not a synthesis/packing report. A logical 32-bit
// port is two SDPB 1024x16 instances with the same address and enables.
struct StorageBankMapping {
    const char* name;
    std::uint32_t instances;
    std::uint32_t banks_per_instance;
    std::uint32_t used_words;
    std::uint32_t meaningful_bits_per_word;
    std::uint32_t guard_bits_per_instance = 0;

    constexpr std::uint32_t blocks() const { return instances * banks_per_instance; }
    constexpr std::uint32_t reserved_bits() const {
        return instances * used_words * meaningful_bits_per_word;
    }
    constexpr std::uint32_t useful_bits() const {
        return reserved_bits() - instances * guard_bits_per_instance;
    }
    constexpr std::uint32_t selected_data_bits() const { return blocks() * 1024u * 16u; }
    constexpr std::uint32_t physical_bits() const {
        return blocks() * TangNano20K::bsram_bits_per_block;
    }
};

struct SystemStorageMap {
    static constexpr const char* profile = "N20K-STORAGE-0.2";
    static constexpr bool mapping_selected = true;
    static constexpr bool physical_fit_validated = false;
    static constexpr std::uint32_t original_bsram_blocks = 12;
    static constexpr std::uint32_t sdram_slot_bsram_blocks = 6 * 2;
    static constexpr std::uint32_t reader_bsram_blocks = 2;
    static constexpr std::uint32_t row_bsram_blocks = 3;
    static constexpr std::uint32_t context_bsram_blocks = BlockRendererResources::context_bsram_blocks;
    static constexpr std::uint32_t reciprocal_seed_bsram_blocks = BlockRendererResources::reciprocal_seed_banks;
    static constexpr std::uint32_t bsram_blocks = original_bsram_blocks +
        sdram_slot_bsram_blocks + reader_bsram_blocks + row_bsram_blocks +
        context_bsram_blocks + reciprocal_seed_bsram_blocks;

    // Each bank has one full-word write port and one synchronous read port.
    // Read/write to the same address on one edge remains forbidden.
    static constexpr std::uint32_t writes_per_bank_per_edge = 1;
    static constexpr std::uint32_t reads_per_bank_per_edge = 1;
    static constexpr std::uint32_t native_read_edges = 1;
    static constexpr std::uint32_t parent_capture_edges = 1;
    static constexpr bool same_address_read_write_allowed = false;

    inline static constexpr std::array<StorageBankMapping, 19> banks{{
        {"tile_color",          1, 1, 1024, 16},
        {"tile_depth_low",      1, 1, 1024, 16},
        {"tile_depth_high",     1, 1, 1024,  8},
        {"texture_cache_copy",  1, 1, 1024, 16},
        {"scanout_fifo",        2, 1, 1024, 16},
        {"host_rx",             1, 1,  256,  8},
        {"host_tx",             1, 1, 1024,  8},
        {"host_packet",         3, 1, 1024,  8},
        {"reply_serializer",    1, 1, 1024,  8},
        {"sdram_client_slot",   6, 2,   16, 32},
        // An unaligned 512-byte read can span 129 aligned words. The extra
        // word supplies alignment guard space, not another four payload bytes.
        {"reader",              1, 2,  129, 32, 32},
        {"row_color",           1, 1,   32, 16},
        {"row_depth_low",       1, 1,   32, 16},
        {"row_depth_high",      1, 1,   32,  8},
        {"fragment_commit",     1, 3,   64, 34},
        {"fragment_factor",     1, 2,   64, 24},
        {"fragment_result",     1, 1,   64, 16},
        {"reciprocal_seed",     1, 3,  256, 35},
        {"reciprocal_gradient", 1, 1,  256, 16}
    }};

    static constexpr std::uint32_t allocated_blocks() {
        std::uint32_t result = 0;
        for (const auto& item : banks) result += item.blocks();
        return result;
    }
    static constexpr bool valid_geometry() {
        for (const auto& item : banks) {
            if (item.instances == 0 || item.banks_per_instance == 0 ||
                item.used_words > 1024 || item.meaningful_bits_per_word == 0 ||
                item.meaningful_bits_per_word > item.banks_per_instance * 16 ||
                item.guard_bits_per_instance > item.used_words * item.meaningful_bits_per_word)
                return false;
        }
        return true;
    }
    static constexpr std::uint32_t useful_bsram_bits() {
        std::uint32_t result = 0;
        for (const auto& item : banks)
            if (item.blocks() != 0) result += item.useful_bits();
        return result;
    }
    static constexpr std::uint32_t reserved_bsram_bits() {
        std::uint32_t result = 0;
        for (const auto& item : banks) result += item.reserved_bits();
        return result;
    }
    static constexpr std::uint32_t selected_data_bits() { return bsram_blocks * 16384u; }
    static constexpr std::uint32_t physical_bits() {
        return bsram_blocks * TangNano20K::bsram_bits_per_block;
    }
    static constexpr std::uint32_t unused_selected_data_bits() {
        return selected_data_bits() - reserved_bsram_bits();
    }

    // Fixed payload arrays that deliberately remain register-mapped. Scalar
    // control/arithmetic state is documented separately; this is not a complete
    // FF total and must not be compared to capacity as a whole-device fit test.
    static constexpr std::uint32_t current_status_bits = 64 * 8;
    static constexpr std::uint32_t latched_status_bits = 64 * 8;
    static constexpr std::uint32_t parser_header_bits = 16 * 8;
    static constexpr std::uint32_t captured_command_bits = 64 * 8;
    static constexpr std::uint32_t decoded_field_bits = 320 * 8;
    static constexpr std::uint32_t reply_source_bits = 92 * 8;
    static constexpr std::uint32_t sdram_mask_bits = 6 * 16 * 4;
    static constexpr std::uint32_t writer_pack_bits = 32 + 4;
    static constexpr std::uint32_t texture_tag_bits = 64 * 12;
    static constexpr std::uint32_t texture_valid_bits = 64;
    static constexpr std::uint32_t capability_rom_bits = 64 * 8;
    static constexpr std::uint32_t context_ready_bits = BlockRendererResources::fragment_contexts;
    static constexpr std::uint32_t context_reason_bits = 2 * BlockRendererResources::fragment_contexts;
    static constexpr std::uint32_t fixed_payload_register_bits = current_status_bits +
        latched_status_bits + parser_header_bits + captured_command_bits +
        decoded_field_bits + reply_source_bits + sdram_mask_bits + writer_pack_bits +
        texture_tag_bits + texture_valid_bits + context_ready_bits + context_reason_bits;
};

static_assert(SystemStorageMap::allocated_blocks() == SystemStorageMap::bsram_blocks);
static_assert(SystemStorageMap::valid_geometry());
static_assert(SystemStorageMap::bsram_blocks == 39);
static_assert(SystemStorageMap::bsram_blocks <= TangNano20K::bsram_blocks);
static_assert(SystemStorageMap::useful_bsram_bits() <= SystemStorageMap::selected_data_bits());
static_assert(SystemStorageMap::reserved_bsram_bits() <= SystemStorageMap::selected_data_bits());

} // namespace nano_gpu::hw
