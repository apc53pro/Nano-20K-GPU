#pragma once

#include "nano_gpu/hw/host_link.hpp"
#include "nano_gpu/hw/scanout.hpp"
#include "nano_gpu/hw/sdram.hpp"
#include "nano_gpu/hw/tile_pipeline.hpp"
#include "nano_gpu/reference.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nano_gpu::hw {

// Host-only cycle observation. Called exactly once per common core edge,
// including command/DMA/idle clocks without a tile datapath edge.
using SystemCycleObserver = std::function<void(std::uint64_t, const FragmentCycle&)>;

struct SystemConfig {
    SdramConfig memory{};
    HostLinkConfig host{};
    ScanoutConfig scanout{};
    std::uint64_t operation_timeout_cycles = 500000000;
};

enum class SystemFault : std::uint32_t {
    None, Transport, Payload, Address, Ownership, InvalidContents, Record,
    ReferenceLimit, Memory, Internal
};

struct SystemStatistics {
    std::uint64_t core_cycles = 0;
    std::uint64_t boot_cycles = 0;
    std::uint64_t commands_completed = 0;
    std::uint64_t passes_completed = 0;
    std::uint64_t copies_completed = 0;
    std::uint64_t presentations_completed = 0;
    std::uint64_t uploaded_bytes = 0;
    std::uint64_t fetched_record_bytes = 0;
    std::uint64_t fetched_bin_bytes = 0;
    std::uint64_t last_pass_cycles = 0;
    std::uint64_t last_submission_cycles = 0;
    std::uint64_t last_present_wait_cycles = 0;
    std::uint64_t last_scanout_observation_cycles = 0;
    std::uint64_t aborts = 0;
    std::uint32_t epoch = 1;
    std::uint32_t completed_sequence = 0;
    std::uint32_t completed_fence_token = 0;
    std::uint32_t completed_present_token = 0;
};

// N20K-SYS-0.1. All device data enters through HostPins and reaches the
// renderer/scanout through the shared timed SDRAM controller. Convenience
// methods below are CPU drivers which serialize and physically transfer bytes.
// The default common core/SDR clock is 100.8 MHz, derived host/pixel clocks
// 25.2 MHz. These are configurable design assumptions, not achieved clocks.
class SystemModel {
public:
    explicit SystemModel(SystemConfig config = {});
    ~SystemModel();
    SystemModel(const SystemModel&) = delete;
    SystemModel& operator=(const SystemModel&) = delete;
    SystemModel(SystemModel&&) noexcept;
    SystemModel& operator=(SystemModel&&) noexcept;

    HostBusResult tick(const HostPins& pins = {});
    // Observer timestamps and fragment IDs are monotonic across hard resets;
    // device core_cycles restarts in each epoch. The observer persists and a
    // reset edge cancels any retained fragment exactly once. Attach before
    // submitting work so the observer sees the matching acceptance events.
    void set_cycle_observer(SystemCycleObserver observer);
    void advance(std::uint64_t cycles);
    void reset();
    void abort();
    bool ready() const;
    bool busy() const;
    bool faulted() const;
    SystemFault fault() const;
    const std::string& fault_message() const;

    void render(const Pass& pass, RenderMode mode = RenderMode::HardwareBlocks);
    void copy_rect(int source, int target, Point source_origin, Point destination_origin,
                   int width, int height);
    void copy_image(const Image& image, int target, Point source_origin,
                    Point destination_origin, int width, int height);
    void queue_present(int target);
    // CPU driver waits for the real vblank PRESENT reply, then one complete
    // scanout observation of that front so SDL can show actual FIFO output.
    void vertical_blank();

    int front_id() const;
    int back_id() const;
    bool presentation_pending() const;
    bool color_valid(int id) const;
    bool depth_valid() const;
    // Diagnostic snapshots have no device-side bandwidth or storage role.
    const std::vector<std::uint16_t>& color(int id) const;
    const std::vector<std::uint16_t>& front_color() const;
    const std::vector<std::uint32_t>& depth() const;
    const std::vector<std::uint16_t>& display_color() const;
    const Statistics& statistics() const;
    const SystemStatistics& system_statistics() const;
    const SdramStatistics& memory_statistics() const;
    const HostLinkStatistics& host_statistics() const;
    const ScanoutStatistics& scanout_statistics() const;
    const SystemConfig& config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void send_bytes(const std::vector<std::uint8_t>& bytes);
    std::vector<std::uint8_t> read_reply(Opcode opcode, std::uint32_t sequence);
    void observe_memory();
};

} // namespace nano_gpu::hw
