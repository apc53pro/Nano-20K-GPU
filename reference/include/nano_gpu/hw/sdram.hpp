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

// Device-side completion metadata. Payload words stay in the client's selected
// synchronous slot RAM until read through request_response_word()/response_word().
struct MemoryResponseInfo {
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
    std::uint64_t prepared_submissions = 0;
    std::uint64_t legacy_submissions = 0;
    std::uint64_t legacy_response_reads = 0;
    std::uint64_t slot_write_words = 0;
    std::uint64_t slot_read_words = 0;
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
    // Each client owns two native SDPB 1024x16 banks, used together as a
    // 16x32 slot. Write payload and read response reuse the same physical RAM.
    static constexpr unsigned storage_bsram_blocks = 2 * memory_client_count;
    explicit SdramController(SdramConfig config = {});
    ~SdramController();
    SdramController(const SdramController&) = delete;
    SdramController& operator=(const SdramController&) = delete;
    SdramController(SdramController&&) noexcept;
    SdramController& operator=(SdramController&&) noexcept;

    // Stage one write word on this client's RAM A ports. It commits only on
    // tick(); at most one staged word per client/core edge is accepted. Masks
    // are sixteen selected four-bit registers. A queued/active/response-owned
    // slot cannot be filled. All words must commit before submit_prepared().
    bool write_request_word(MemoryClient client, unsigned index,
                            std::uint32_t data, std::uint8_t mask);
    // Capture only metadata; data/masks arrays in this compatibility carrier
    // are ignored. Reads need no input words. Writes require a complete fill.
    bool submit_prepared(MemoryClient client, const MemoryRequest& metadata);
    // Cancel an unsubmitted fill during recovery. Queued/active requests and
    // completed responses must retire instead of being discarded here.
    void discard_request_words(MemoryClient client);
    bool response_ready(MemoryClient client) const;
    MemoryResponseInfo response_info(MemoryClient client) const;
    // Stage one synchronous B-port read. Validity clears immediately, becomes
    // true after tick(), and persists until another read or release. Consumers
    // sample the established result before advancing their following edge.
    bool request_response_word(MemoryClient client, unsigned index);
    bool response_word_valid(MemoryClient client) const;
    unsigned response_word_offset(MemoryClient client) const;
    std::uint32_t response_word(MemoryClient client) const;
    void release_response(MemoryClient client);

    // Host-only standalone-test adapters. submit() drives one payload word
    // per controller edge into the same physical slot before queueing a write;
    // take_response() reads an observer shadow. GPU clients use only the ports
    // above. Adapter use is reported separately and must be zero in the system.
    bool submit(MemoryClient client, const MemoryRequest& request);
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
