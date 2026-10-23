#include "nano_gpu/hw/sdram.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace nano_gpu::hw {
namespace {

constexpr std::uint32_t sdram_bytes = 8u * 1024u * 1024u;
constexpr unsigned native_burst_words = 8;

std::size_t client_index(MemoryClient client) {
    const auto value = static_cast<std::size_t>(client);
    if (value >= memory_client_count) throw std::invalid_argument("invalid SDRAM client");
    return value;
}

void validate_address(std::uint32_t address) {
    if ((address & 3u) != 0 || address >= sdram_bytes)
        throw std::out_of_range("SDRAM word address must be aligned inside 8 MiB");
}

std::uint32_t byte_mask(std::uint8_t enables) {
    std::uint32_t result = 0;
    for (unsigned byte = 0; byte < 4; ++byte)
        if ((enables & (1u << byte)) != 0) result |= 0xffu << (8 * byte);
    return result;
}

unsigned byte_count(std::uint8_t mask) {
    unsigned result = 0;
    for (unsigned byte = 0; byte < 4; ++byte) result += (mask >> byte) & 1u;
    return result;
}

} // namespace

struct SdramController::Impl {
    enum class Phase {
        PowerWait, InitPrecharge, InitRefreshOne, InitRefreshTwo, InitMode,
        Ready, Prepare, ReadLatency, ReadData, WriteData, WriteRecovery,
        RefreshPrecharge, RefreshRecovery
    };
    struct Slot {
        bool occupied = false;
        bool queued = false;
        bool response_valid = false;
        std::uint64_t queued_at = 0;
        MemoryRequest request;
        MemoryResponse response;
    };
    struct Bank {
        bool open = false;
        std::uint16_t row = 0;
        std::uint64_t active_at = 0;
        std::uint64_t activate_after = 0;
        std::uint64_t column_after = 0;
        std::uint64_t precharge_after = 0;
    };

    SdramConfig selected;
    SdramStatistics counts;
    SdramPins signal;
    std::vector<std::uint32_t> words = std::vector<std::uint32_t>(sdram_bytes / 4);
    std::vector<std::uint8_t> defined = std::vector<std::uint8_t>(sdram_bytes / 4);
    std::array<Slot, memory_client_count> slots;
    std::array<Bank, 4> banks;
    Phase phase = Phase::PowerWait;
    bool ready = false;
    std::uint64_t wait_until = 0;
    std::uint64_t next_refresh = 0;
    std::uint64_t last_refresh = 0;
    std::uint64_t refresh_guard = 0;
    std::optional<std::size_t> active;
    std::uint64_t service_started = 0;
    std::size_t round_robin = 1;
    unsigned scanout_streak = 0;
    unsigned processed = 0;
    unsigned burst_first = 0;
    unsigned burst_useful = 0;
    unsigned burst_beat = 0;
    std::uint32_t burst_base = 0;
    bool burst_row_accounted = false;
    bool have_bus_direction = false;
    bool last_bus_write = false;
    std::uint64_t last_data_edge = 0;

    explicit Impl(SdramConfig config) : selected(config) {
        if (selected.core_hz == 0 || selected.core_hz > 166000000 ||
            (selected.cas_latency != 2 && selected.cas_latency != 3) ||
            selected.t_rp == 0 || selected.t_rcd == 0 || selected.t_rfc == 0 ||
            selected.t_mrd == 0 || selected.t_wr == 0 || selected.t_ras == 0 ||
            selected.t_rc < static_cast<std::uint64_t>(selected.t_ras) + selected.t_rp ||
            selected.scanout_priority_limit == 0)
            throw std::invalid_argument("invalid SDRAM clock or controller timing configuration");
        const auto minimum_power_wait = (selected.core_hz + 9999) / 10000;
        const auto maximum_refresh_interval = selected.core_hz / 64000;
        if (selected.power_up_cycles == 0) selected.power_up_cycles = minimum_power_wait;
        if (selected.refresh_interval_cycles == 0)
            selected.refresh_interval_cycles = maximum_refresh_interval;
        if (selected.power_up_cycles < minimum_power_wait ||
            selected.refresh_interval_cycles == 0 ||
            selected.refresh_interval_cycles > maximum_refresh_interval)
            throw std::invalid_argument("SDRAM initialization/refresh exceeds documented time limits");

        // A request has at most three aligned BL8 transfers. Reserve a complete
        // worst-case request, bank close, and refresh entry before each deadline.
        // This conservative controller never accepts an unlimited burst that
        // could defer mandatory refresh indefinitely.
        const std::uint64_t segment = static_cast<std::uint64_t>(selected.t_rp) + selected.t_rcd +
            selected.t_ras + selected.t_rc + selected.cas_latency + native_burst_words +
            selected.t_wr + selected.read_to_write + selected.write_to_read + 4;
        refresh_guard = 3 * segment + selected.t_rp + 2;
        if (selected.refresh_interval_cycles <= refresh_guard + selected.t_rfc + selected.t_rp)
            throw std::invalid_argument("refresh interval leaves no bounded transaction service window");
    }

    std::uint64_t now() const { return counts.cycles; }

    std::uint32_t observed_word(std::uint32_t index) const {
        const auto mask = byte_mask(defined[index]);
        return (words[index] & mask) | (0xcdcdcdcdu & ~mask);
    }

    void command(SdramCommand operation, unsigned bank = 0, unsigned address = 0) {
        if (signal.command != SdramCommand::Nop)
            throw std::logic_error("two SDRAM commands issued on one edge");
        signal.command = operation;
        signal.bank = static_cast<std::uint8_t>(bank);
        signal.address = static_cast<std::uint16_t>(address);
    }

    bool can_precharge(const Bank& bank) const {
        return !bank.open || now() >= bank.precharge_after;
    }

    void precharge(unsigned bank) {
        auto& b = banks[bank];
        if (!can_precharge(b)) throw std::logic_error("SDRAM tRAS/tWR precharge violation");
        command(SdramCommand::Precharge, bank);
        b.open = false;
        b.activate_after = std::max(b.activate_after, now() + selected.t_rp);
        ++counts.precharges;
    }

    void precharge_all() {
        for (const auto& bank : banks)
            if (!can_precharge(bank)) throw std::logic_error("SDRAM PRECHARGE ALL before tRAS/tWR");
        command(SdramCommand::PrechargeAll, 0, 1u << 10);
        for (auto& bank : banks) {
            bank.open = false;
            bank.activate_after = std::max(bank.activate_after, now() + selected.t_rp);
        }
        ++counts.precharges;
    }

    void refresh(bool periodic) {
        for (const auto& bank : banks)
            if (bank.open || now() < bank.activate_after)
                throw std::logic_error("SDRAM AUTO REFRESH requires precharged banks and tRP/tRC");
        command(SdramCommand::AutoRefresh);
        ++counts.refreshes;
        if (last_refresh != 0)
            counts.max_refresh_gap_cycles = std::max(counts.max_refresh_gap_cycles, now() - last_refresh);
        last_refresh = now();
        for (auto& bank : banks) bank.activate_after = now() + selected.t_rfc;
        if (periodic) {
            if (now() > next_refresh) counts.refresh_late_cycles += now() - next_refresh;
            // Start the next retention deadline at the actual command edge.
            // Refresh may run early to reserve a bounded request service window,
            // but variable traffic must never extend a command-to-command gap.
            next_refresh = now() + selected.refresh_interval_cycles;
        }
        wait_until = now() + selected.t_rfc;
    }

    std::optional<std::size_t> select_client() {
        constexpr std::size_t scanout = static_cast<std::size_t>(MemoryClient::Scanout);
        const bool display_pending = slots[scanout].queued;
        if (display_pending && scanout_streak < selected.scanout_priority_limit) {
            ++scanout_streak;
            return scanout;
        }
        for (std::size_t attempt = 0; attempt < memory_client_count - 1; ++attempt) {
            const auto candidate = 1 + (round_robin - 1 + attempt) % (memory_client_count - 1);
            if (slots[candidate].queued) {
                round_robin = candidate == memory_client_count - 1 ? 1 : candidate + 1;
                scanout_streak = 0;
                return candidate;
            }
        }
        if (display_pending) {
            scanout_streak = std::min(scanout_streak + 1, selected.scanout_priority_limit);
            return scanout;
        }
        return std::nullopt;
    }

    void choose_burst() {
        const auto& request = slots[*active].request;
        const auto first_word = request.address / 4 + processed;
        burst_base = first_word & ~(native_burst_words - 1u);
        burst_first = first_word & (native_burst_words - 1u);
        burst_useful = std::min(native_burst_words - burst_first, request.words - processed);
        burst_beat = 0;
        burst_row_accounted = false;
    }

    void complete() {
        auto& slot = slots[*active];
        slot.response_valid = true;
        auto& client = counts.clients[*active];
        ++client.completed;
        const auto service = now() - service_started + 1;
        client.service_cycles += service;
        client.max_service_cycles = std::max(client.max_service_cycles, service);
        active.reset();
        phase = Phase::Ready;
    }

    void data_beat(bool writing) {
        auto& slot = slots[*active];
        auto& client = counts.clients[*active];
        const bool useful = burst_beat >= burst_first && burst_beat < burst_first + burst_useful;
        const auto word = burst_base + burst_beat;
        const auto index = useful ? processed + burst_beat - burst_first : 0;
        signal.data_valid = true;
        signal.data_write = writing;
        if (writing) {
            ++counts.physical_write_beats;
            const auto enables = useful ? slot.request.masks[index] : std::uint8_t{0};
            signal.dqm = static_cast<std::uint8_t>((~enables) & 0xfu);
            signal.data = useful ? slot.request.data[index] : 0;
            const auto mask = byte_mask(enables);
            words[word] = (words[word] & ~mask) | (signal.data & mask);
            defined[word] = static_cast<std::uint8_t>(defined[word] | enables);
            client.bytes_written += byte_count(enables);
            auto& bank = banks[(word >> 8) & 3u];
            bank.precharge_after = std::max(bank.precharge_after, now() + selected.t_wr);
        } else {
            ++counts.physical_read_beats;
            signal.dqm = 0;
            signal.data = observed_word(word);
            if (useful) {
                slot.response.data[index] = signal.data;
                client.bytes_read += 4;
                if (defined[word] != 0xf) ++counts.uninitialized_read_words;
            }
        }
        have_bus_direction = true;
        last_bus_write = writing;
        last_data_edge = now();
        if (++burst_beat == native_burst_words) {
            processed += burst_useful;
            if (processed == slot.request.words) {
                if (writing) {
                    wait_until = now() + selected.t_wr;
                    phase = Phase::WriteRecovery;
                } else complete();
            } else {
                choose_burst();
                phase = Phase::Prepare;
            }
        }
    }

    void prepare() {
        auto& slot = slots[*active];
        const unsigned bank_number = (burst_base >> 8) & 3u;
        const auto row = static_cast<std::uint16_t>(burst_base >> 10);
        auto& bank = banks[bank_number];
        if (!burst_row_accounted) {
            if (bank.open && bank.row == row) ++counts.row_hits;
            else ++counts.row_misses;
            burst_row_accounted = true;
        }
        if (bank.open && bank.row != row) {
            if (can_precharge(bank)) precharge(bank_number);
            return;
        }
        if (!bank.open) {
            if (now() < bank.activate_after) return;
            command(SdramCommand::Active, bank_number, row);
            bank.open = true;
            bank.row = row;
            bank.active_at = now();
            bank.column_after = now() + selected.t_rcd;
            bank.precharge_after = now() + selected.t_ras;
            bank.activate_after = now() + selected.t_rc;
            ++counts.activates;
            return;
        }
        if (now() < bank.column_after) return;
        if (have_bus_direction && last_bus_write != slot.request.write) {
            const auto turnaround = last_bus_write ? selected.write_to_read : selected.read_to_write;
            if (now() < last_data_edge + turnaround) return;
        }
        const auto column = burst_base & 255u;
        command(slot.request.write ? SdramCommand::Write : SdramCommand::Read, bank_number, column);
        if (slot.request.write) {
            ++counts.write_commands;
            phase = Phase::WriteData;
            data_beat(true); // SDR WRITE captures its first DQ word on this edge.
        } else {
            ++counts.read_commands;
            wait_until = now() + selected.cas_latency;
            phase = Phase::ReadLatency;
        }
    }

    void tick() {
        ++counts.cycles;
        signal = {};
        signal.cke = true;
        // Keep reads unmasked ahead of their CAS-delayed DQ beats. DQM must
        // not first go low when data appears: SDR read masking is pipelined.
        // Only WRITE edges override these low byte masks.
        signal.dqm = 0;
        if (!ready) ++counts.initialization_cycles;
        switch (phase) {
        case Phase::PowerWait:
            if (now() >= selected.power_up_cycles) {
                precharge_all();
                wait_until = now() + selected.t_rp;
                phase = Phase::InitPrecharge;
            }
            break;
        case Phase::InitPrecharge:
            if (now() >= wait_until) { refresh(false); phase = Phase::InitRefreshOne; }
            break;
        case Phase::InitRefreshOne:
            if (now() >= wait_until) { refresh(false); phase = Phase::InitRefreshTwo; }
            break;
        case Phase::InitRefreshTwo:
            if (now() >= wait_until) {
                command(SdramCommand::LoadMode, 0, (selected.cas_latency << 4) | 3u);
                wait_until = now() + selected.t_mrd;
                phase = Phase::InitMode;
            }
            break;
        case Phase::InitMode:
            if (now() >= wait_until) {
                ready = true;
                next_refresh = last_refresh + selected.refresh_interval_cycles;
                phase = Phase::Ready;
            }
            break;
        case Phase::Ready:
            if (now() + refresh_guard >= next_refresh) {
                bool close_ready = true;
                for (const auto& bank : banks) close_ready = close_ready && can_precharge(bank);
                if (close_ready) {
                    precharge_all();
                    wait_until = now() + selected.t_rp;
                    for (const auto& bank : banks)
                        wait_until = std::max(wait_until, bank.activate_after);
                    phase = Phase::RefreshPrecharge;
                    ++counts.refresh_busy_cycles;
                }
                break;
            }
            active = select_client();
            if (active) {
                auto& slot = slots[*active];
                auto& client = counts.clients[*active];
                slot.queued = false;
                slot.response = {};
                slot.response.words = slot.request.words;
                slot.response.tag = slot.request.tag;
                const auto wait = now() - slot.queued_at;
                client.queue_wait_cycles += wait;
                client.max_queue_wait_cycles = std::max(client.max_queue_wait_cycles, wait);
                service_started = now();
                processed = 0;
                choose_burst();
                phase = Phase::Prepare;
            } else ++counts.idle_cycles;
            break;
        case Phase::Prepare:
            prepare();
            break;
        case Phase::ReadLatency:
            if (now() >= wait_until) {
                phase = Phase::ReadData;
                data_beat(false);
            }
            break;
        case Phase::ReadData:
            data_beat(false);
            break;
        case Phase::WriteData:
            data_beat(true);
            break;
        case Phase::WriteRecovery:
            if (now() >= wait_until) complete();
            break;
        case Phase::RefreshPrecharge:
            ++counts.refresh_busy_cycles;
            if (now() >= wait_until) { refresh(true); phase = Phase::RefreshRecovery; }
            break;
        case Phase::RefreshRecovery:
            ++counts.refresh_busy_cycles;
            if (now() >= wait_until) phase = Phase::Ready;
            break;
        }
    }
};

SdramController::SdramController(SdramConfig config) : impl_(std::make_unique<Impl>(config)) {}
SdramController::~SdramController() = default;
SdramController::SdramController(SdramController&&) noexcept = default;
SdramController& SdramController::operator=(SdramController&&) noexcept = default;

bool SdramController::submit(MemoryClient client, const MemoryRequest& request) {
    const auto index = client_index(client);
    validate_address(request.address);
    if (request.words == 0 || request.words > memory_burst_words ||
        static_cast<std::uint64_t>(request.address) + 4u * request.words > sdram_bytes)
        throw std::out_of_range("SDRAM request extent exceeds the bounded 16-word port");
    if (request.write) {
        for (unsigned word = 0; word < request.words; ++word)
            if ((request.masks[word] & 0xf0u) != 0)
                throw std::out_of_range("SDRAM byte enable exceeds four DQM lanes");
    }
    auto& p = *impl_;
    auto& slot = p.slots[index];
    if (slot.occupied) { ++p.counts.clients[index].rejected_busy; return false; }
    slot.occupied = true;
    slot.queued = true;
    slot.response_valid = false;
    slot.request = request;
    slot.queued_at = p.now();
    ++p.counts.clients[index].requests;
    return true;
}

bool SdramController::response_ready(MemoryClient client) const {
    return impl_->slots[client_index(client)].response_valid;
}

MemoryResponse SdramController::take_response(MemoryClient client) {
    auto& slot = impl_->slots[client_index(client)];
    if (!slot.response_valid) throw std::logic_error("SDRAM response is not valid");
    auto result = slot.response;
    slot = {};
    return result;
}

void SdramController::tick() { impl_->tick(); }
bool SdramController::initialized() const { return impl_->ready; }
bool SdramController::idle() const {
    if (!impl_->ready || impl_->phase != Impl::Phase::Ready) return false;
    for (const auto& slot : impl_->slots) if (slot.occupied) return false;
    return true;
}
std::uint64_t SdramController::cycles() const { return impl_->counts.cycles; }
const SdramStatistics& SdramController::statistics() const { return impl_->counts; }
const SdramConfig& SdramController::config() const { return impl_->selected; }
const SdramPins& SdramController::pins() const { return impl_->signal; }
std::uint32_t SdramController::inspect_word(std::uint32_t address) const {
    validate_address(address);
    return impl_->observed_word(address / 4);
}

} // namespace nano_gpu::hw
