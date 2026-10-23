#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace nano_gpu::hw {

enum class MemoryClient { Scanout, Host, Command, Tile, Texture, Copy, Count };
constexpr std::size_t memory_client_count = static_cast<std::size_t>(MemoryClient::Count);
constexpr unsigned memory_burst_words = 16;

// Request addresses are byte addresses. One bounded request and one unconsumed
// response per client reserve the same slot; submit() returns false while owned.
struct MemoryRequest {
    bool write = false;
    std::uint32_t address = 0;
    unsigned words = 0;
    std::array<std::uint32_t, memory_burst_words> data{};
    // Low four bits enable individual bytes. Full writes explicitly use 0xf.
    std::array<std::uint8_t, memory_burst_words> masks{};
    std::uint32_t tag = 0;
};

struct MemoryResponse {
    std::array<std::uint32_t, memory_burst_words> data{};
    unsigned words = 0;
    std::uint32_t tag = 0;
};

struct SdramConfig {
    std::uint64_t core_hz = 100800000;
    // Project controller timing assumptions, not verified speed-bin limits.
    unsigned cas_latency = 3;
    unsigned t_rp = 3;
    unsigned t_rcd = 3;
    unsigned t_rfc = 9;
    unsigned t_mrd = 3;
    unsigned t_wr = 3;
    unsigned t_ras = 6;
    unsigned t_rc = 9;
    unsigned read_to_write = 2;
    unsigned write_to_read = 2;
    // Zero derives >=100 us and <=15.625 us respectively from core_hz.
    std::uint64_t power_up_cycles = 0;
    std::uint64_t refresh_interval_cycles = 0;
    unsigned scanout_priority_limit = 4;
};

struct SdramClientStatistics {
    std::uint64_t requests = 0;
    std::uint64_t completed = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t queue_wait_cycles = 0;
    std::uint64_t max_queue_wait_cycles = 0;
    std::uint64_t service_cycles = 0;
    std::uint64_t max_service_cycles = 0;
    std::uint64_t rejected_busy = 0;
};

struct SdramStatistics {
    std::uint64_t cycles = 0;
    std::uint64_t initialization_cycles = 0;
    std::uint64_t refreshes = 0;
    std::uint64_t refresh_busy_cycles = 0;
    std::uint64_t refresh_late_cycles = 0;
    std::uint64_t max_refresh_gap_cycles = 0;
    std::uint64_t row_hits = 0;
    std::uint64_t row_misses = 0;
    std::uint64_t activates = 0;
    std::uint64_t precharges = 0;
    std::uint64_t read_commands = 0;
    std::uint64_t write_commands = 0;
    std::uint64_t physical_read_beats = 0;
    std::uint64_t physical_write_beats = 0;
    std::uint64_t uninitialized_read_words = 0;
    std::uint64_t idle_cycles = 0;
    std::array<SdramClientStatistics, memory_client_count> clients{};
};

enum class SdramCommand { Nop, PrechargeAll, AutoRefresh, LoadMode, Active, Precharge, Read, Write };

struct SdramPins {
    SdramCommand command = SdramCommand::Nop;
    bool cke = false;
    std::uint8_t bank = 0;
    std::uint16_t address = 0;
    bool data_valid = false;
    bool data_write = false;
    std::uint32_t data = 0;
    // Native SDRAM DQM is active-high: one masks the corresponding byte.
    std::uint8_t dqm = 0xf;
};

// QN88's 8 MiB SDR device: 4 banks x2048 rows x256 columns x32 bits.
// Word mapping: [row(11), bank(2), column(8)]. Selected native mode is CL3
// (CL2 is configurable), sequential BL8, common SDR/controller clock.
// All content changes through accepted, timed requests; inspect_word is a
// diagnostic observer and is never a renderer/scanout memory port.
class SdramController {
public:
    explicit SdramController(SdramConfig config = {});
    ~SdramController();
    SdramController(const SdramController&) = delete;
    SdramController& operator=(const SdramController&) = delete;
    SdramController(SdramController&&) noexcept;
    SdramController& operator=(SdramController&&) noexcept;

    bool submit(MemoryClient client, const MemoryRequest& request);
    bool response_ready(MemoryClient client) const;
    MemoryResponse take_response(MemoryClient client);
    // One rising edge: commands, DQ beats, memory commits and output registers.
    // Callers sample established outputs before advancing the common edge.
    void tick();
    bool initialized() const;
    bool idle() const;
    std::uint64_t cycles() const;
    const SdramStatistics& statistics() const;
    const SdramConfig& config() const;
    const SdramPins& pins() const;
    // Unwritten bytes deterministically expose 0xcd poison rather than a
    // fictitious SDRAM zero-initialization guarantee.
    std::uint32_t inspect_word(std::uint32_t byte_address) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nano_gpu::hw
