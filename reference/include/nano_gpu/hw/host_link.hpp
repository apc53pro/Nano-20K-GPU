#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace nano_gpu::hw {

constexpr std::uint8_t protocol_major = 0;
constexpr std::uint8_t protocol_minor = 2;

enum class Opcode : std::uint16_t {
    GetCaps = 1, GetStatus = 2, WriteHeap = 3, SubmitPass = 4,
    CopyRect = 5, Fence = 6, Present = 7, ReadMemory = 8
};

constexpr std::size_t host_max_payload_bytes = 1024;
struct HostCommand {
    Opcode opcode = Opcode::GetCaps;
    std::uint32_t sequence = 0;
    std::uint32_t payload_size = 0;
    // Diagnostic shadow only. GPU execution must use the clocked payload read
    // port below; this array is not an additional device RAM or a free port.
    std::array<std::uint8_t, host_max_payload_bytes> payload{};
};

struct HostLinkConfig {
    unsigned clock_divider = 4;
    unsigned transfer_phase = 3;
    unsigned setup_cycles = 1;
    unsigned hold_cycles = 1;
    unsigned rx_fifo_capacity = 256;
    unsigned command_fifo_capacity = 2;
    unsigned response_fifo_capacity = 1024;
};

// Project-selected synchronous link, not a verified board pin assignment or
// analogue electrical model. One shared 8-bit data bus transfers at most one
// byte on the selected edge of the derived host clock. Register 0 writes packet
// bytes or reads reply bytes; registers 1..63 read sideband status even when
// packet parsing/command execution is blocked. Registers 64..127 expose the
// immutable capability bytes at offsets 0..63. Abort/reset are out of band.
struct HostPins {
    bool write_valid = false;
    bool read = false;
    std::uint8_t data = 0;
    std::uint8_t register_address = 0;
    bool abort = false;
    bool reset = false;
};

struct HostBusResult {
    bool transfer_edge = false;
    bool write_accepted = false;
    bool read_valid = false;
    bool write_ready = false;
    std::uint8_t read_data = 0;
};

enum class HostFault : std::uint8_t {
    None, BusDirection, RegisterAddress, Setup, Hold, UnstableWrite,
    Magic, Version, Opcode, Length, Sequence, RecoveryPending
};

struct HostLinkStatistics {
    std::uint64_t cycles = 0;
    std::uint64_t transfer_edges = 0;
    std::uint64_t rx_bytes = 0;
    std::uint64_t tx_bytes = 0;
    std::uint64_t status_reads = 0;
    std::uint64_t capability_reads = 0;
    std::uint64_t backpressure_cycles = 0;
    std::uint64_t backpressure_edges = 0;
    std::uint64_t parser_bytes = 0;
    std::uint64_t parser_stall_cycles = 0;
    std::uint64_t commands = 0;
    std::uint64_t setup_violations = 0;
    std::uint64_t hold_violations = 0;
    std::uint64_t unstable_writes = 0;
    std::uint64_t bus_direction_violations = 0;
    std::uint64_t protocol_faults = 0;
    std::uint64_t aborts = 0;
    std::uint64_t resets = 0;
    unsigned rx_high_water = 0;
    unsigned command_high_water = 0;
    unsigned response_high_water = 0;
};

class HostLink {
public:
    // RX, TX, and three packet banks use selected SDPB 1024x16 instances.
    // Bytes occupy the low eight bits; spare bits do not create extra ports.
    static constexpr unsigned storage_bsram_blocks = 5;
    explicit HostLink(HostLinkConfig config = {});
    ~HostLink();
    HostLink(const HostLink&) = delete;
    HostLink& operator=(const HostLink&) = delete;
    HostLink(HostLink&&) noexcept;
    HostLink& operator=(HostLink&&) noexcept;

    // Each call is one core edge. The decoder consumes only bytes established
    // before this edge; a newly accepted byte cannot execute immediately.
    // Set up controls/data before a transfer edge. Hold for hold_cycles after
    // an accepted write. With continuous write_valid, a backpressured byte
    // must remain stable; deasserting valid withdraws an unaccepted attempt.
    HostBusResult tick(const HostPins& pins = {});
    bool command_ready() const;
    const HostCommand& front_command() const;
    void pop_command();
    // Stage one synchronous read of the current command's packet bank for the
    // next tick. Only metadata in front_command() is a combinational interface.
    // The byte/offset become valid after that tick and must be sampled before
    // the next tick (the same old-output discipline as other native memories).
    bool request_payload_byte(unsigned offset);
    bool payload_byte_valid() const;
    std::uint8_t payload_byte() const;
    unsigned payload_byte_offset() const;
    // An accepted register-1 read atomically latches all 64 current status
    // bytes. Subsequent reads use that snapshot until register 1 is read again.
    void set_status(const std::array<std::uint8_t, 64>& snapshot);
    // Configure the read-only capability bank once; subsequent identical
    // configuration is harmless, while changing a live bank is rejected.
    void set_capabilities(const std::array<std::uint8_t, 64>& capabilities);
    // Register 1 ORs in bit7=fault, bit6=RX full, bit5=command FIFO full,
    // bit4=reply available. Bits0..3 and all other status bytes are caller-owned.
    // Stage at most one producer byte per core edge. It commits on tick(); a
    // consumer cannot observe a newly staged byte on that same edge. False
    // means either a staged producer already owns this edge or FIFO capacity.
    bool push_response_byte(std::uint8_t byte);
    // Occupancy includes the reserved producer byte and registered read stages.
    unsigned response_size() const;
    unsigned response_free() const;
    unsigned rx_size() const;
    unsigned command_size() const;

    bool faulted() const;
    HostFault fault() const;
    bool abort_requested() const;
    bool reset_requested() const;
    std::uint32_t epoch() const;
    // Recovery flushes all old partial/queued input and reply bytes, resets
    // sequence tracking, and establishes a new nonzero epoch. Statistics and
    // clock phase remain cumulative. Caller must quiesce the host and drain
    // already issued memory operations before resuming a recovered session.
    void recover(std::uint32_t epoch);
    void reset();
    const HostLinkStatistics& statistics() const;
    const HostLinkConfig& config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nano_gpu::hw
