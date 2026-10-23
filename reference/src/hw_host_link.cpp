#include "nano_gpu/hw/host_link.hpp"
#include "nano_gpu/hw/bsram.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace nano_gpu::hw {
namespace {

std::uint16_t read16(const std::array<std::uint8_t, 16>& bytes, unsigned offset) {
    return static_cast<std::uint16_t>(bytes[offset] |
        (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}
std::uint32_t read32(const std::array<std::uint8_t, 16>& bytes, unsigned offset) {
    std::uint32_t result = 0;
    for (unsigned byte = 0; byte < 4; ++byte)
        result |= static_cast<std::uint32_t>(bytes[offset + byte]) << (8 * byte);
    return result;
}
bool known_opcode(std::uint16_t opcode) {
    return opcode >= static_cast<std::uint16_t>(Opcode::GetCaps) &&
           opcode <= static_cast<std::uint16_t>(Opcode::ReadMemory);
}

} // namespace

struct HostLink::Impl {
    explicit Impl(HostLinkConfig selected) : config(selected) {}

    HostLinkConfig config;
    HostLinkStatistics counters;
    Sdp1kx16 receive_bank, response_bank;
    std::array<Sdp1kx16, 3> packet_memory;
    unsigned receive_head = 0, receive_tail = 0, receive_size = 0;
    bool receive_read_pending = false;
    unsigned response_head = 0, response_tail = 0, response_size = 0;
    bool response_write_pending = false;
    std::uint8_t response_write_byte = 0;
    bool response_read_pending = false, response_prefetched = false;
    std::uint8_t response_prefetch_byte = 0;
    // Three bounded packet banks: at most two queued plus one decoder-owned.
    // Publication swaps bank ownership, rather than copying 1 KiB in one edge.
    std::array<HostCommand, 3> packet_banks{};
    std::array<unsigned, 2> command_banks{};
    unsigned staging_bank = 2;
    unsigned command_head = 0, command_tail = 0, command_size = 0;
    std::array<std::uint8_t, 64> status{};
    std::array<std::uint8_t, 64> capabilities{};
    bool capabilities_configured = false;
    std::array<std::uint8_t, 64> latched_status{};
    bool status_latched = false;
    std::array<std::uint8_t, 16> header{};
    unsigned header_size = 0;
    unsigned payload_received = 0;
    bool header_decoded = false, packet_complete = false;
    std::uint32_t last_sequence = 0, epoch = 1;
    HostFault fault = HostFault::None;
    HostPins previous;
    unsigned write_stable = 0, read_stable = 0;
    bool waiting_write = false;
    std::uint8_t waiting_data = 0, waiting_register = 0;
    std::uint8_t held_data = 0, held_register = 0;
    std::uint64_t hold_until = 0;
    bool abort_requested = false, reset_requested = false;
    bool payload_request_pending = false, payload_valid = false;
    unsigned payload_request_bank = 0, payload_request_offset = 0;
    unsigned payload_output_bank = 0, payload_output_offset = 0;
    BsramCycle receive_port, response_port;
    std::array<BsramCycle, 3> packet_ports{};

    HostCommand& staging() { return packet_banks[staging_bank]; }
    unsigned total_response_size() const {
        return response_size + response_write_pending + response_read_pending + response_prefetched;
    }
    void start_ports() {
        receive_port = {};
        response_port = {};
        packet_ports = {};
    }
    void clock_ports() {
        if (payload_request_pending) {
            auto& port = packet_ports[payload_request_bank];
            port.read_enable = true;
            port.read_address = static_cast<std::uint16_t>(payload_request_offset);
            payload_output_bank = payload_request_bank;
            payload_output_offset = payload_request_offset;
        }
        payload_valid = payload_request_pending;
        payload_request_pending = false;
        receive_bank.tick(receive_port);
        response_bank.tick(response_port);
        for (unsigned bank = 0; bank < packet_memory.size(); ++bank)
            packet_memory[bank].tick(packet_ports[bank]);
    }

    void fail(HostFault reason) {
        if (fault == HostFault::None) {
            fault = reason;
            ++counters.protocol_faults;
        }
    }
    void flush() {
        receive_head = receive_tail = receive_size = 0;
        receive_read_pending = false;
        response_head = response_tail = response_size = 0;
        response_write_pending = response_read_pending = response_prefetched = false;
        command_head = command_tail = command_size = 0;
        header_size = payload_received = 0;
        header_decoded = packet_complete = false;
        write_stable = read_stable = 0;
        waiting_write = false;
        hold_until = 0;
        status_latched = false;
        payload_request_pending = payload_valid = false;
    }
    void decode_header() {
        constexpr std::array<std::uint8_t, 4> magic{{0x4e, 0x47, 0x50, 0x55}};
        if (!std::equal(magic.begin(), magic.end(), header.begin())) {
            fail(HostFault::Magic);
        } else if (header[4] != 0 || header[5] != 1) {
            fail(HostFault::Version);
        } else if (!known_opcode(read16(header, 6))) {
            fail(HostFault::Opcode);
        } else if (read32(header, 8) > host_max_payload_bytes) {
            fail(HostFault::Length);
        } else if (read32(header, 12) == 0 || read32(header, 12) <= last_sequence) {
            fail(HostFault::Sequence);
        } else {
            staging().opcode = static_cast<Opcode>(read16(header, 6));
            staging().payload_size = read32(header, 8);
            staging().sequence = read32(header, 12);
            last_sequence = staging().sequence;
            payload_received = 0;
            header_decoded = true;
            packet_complete = staging().payload_size == 0;
        }
    }
    void decoder_edge() {
        if (fault != HostFault::None) return;
        if (packet_complete) {
            if (command_size == config.command_fifo_capacity) {
                ++counters.parser_stall_cycles;
                return;
            }
            // A complete staged bank is published on a later edge. Only its
            // bounded bank selector changes ownership at this boundary.
            command_banks[command_tail] = staging_bank;
            command_tail = (command_tail + 1) % config.command_fifo_capacity;
            ++command_size;
            for (unsigned candidate = 0; candidate < packet_banks.size(); ++candidate) {
                bool queued = false;
                for (unsigned position = 0; position < command_size; ++position) {
                    const unsigned slot = (command_head + position) % config.command_fifo_capacity;
                    queued = queued || command_banks[slot] == candidate;
                }
                if (!queued) { staging_bank = candidate; break; }
            }
            ++counters.commands;
            counters.command_high_water = std::max(counters.command_high_water, command_size);
            header_size = payload_received = 0;
            header_decoded = packet_complete = false;
            return;
        }
        if (receive_read_pending) {
            const auto byte = static_cast<std::uint8_t>(receive_bank.output());
            receive_read_pending = false;
            ++counters.parser_bytes;
            if (!header_decoded) {
                header[header_size++] = byte;
                if (header_size == header.size()) decode_header();
            } else {
                auto& port = packet_ports[staging_bank];
                port.write_enable = true;
                port.write_address = static_cast<std::uint16_t>(payload_received);
                port.write_data = byte;
                // Host-only observer. Execution has no permission to read this
                // mirror in place of the selected memory's synchronous port.
                staging().payload[payload_received++] = byte;
                if (payload_received == staging().payload_size) packet_complete = true;
            }
        }
        if (!packet_complete && fault == HostFault::None && receive_size != 0) {
            receive_port.read_enable = true;
            receive_port.read_address = static_cast<std::uint16_t>(receive_head);
            receive_head = (receive_head + 1) % config.rx_fifo_capacity;
            --receive_size;
            receive_read_pending = true;
        }
    }
    void response_edge() {
        // Consume an established RAM output only on a later common edge.
        // The explicit prefetch register can retain the head while the host
        // pauses. A second byte may wait in the native RAM output register.
        if (response_read_pending && !response_prefetched) {
            response_prefetch_byte = static_cast<std::uint8_t>(response_bank.output());
            response_prefetched = true;
            response_read_pending = false;
        }
        if (!response_read_pending && response_size != 0) {
            response_port.read_enable = true;
            response_port.read_address = static_cast<std::uint16_t>(response_head);
            response_head = (response_head + 1) % config.response_fifo_capacity;
            --response_size;
            response_read_pending = true;
        }
        if (response_write_pending) {
            response_port.write_enable = true;
            response_port.write_address = static_cast<std::uint16_t>(response_tail);
            response_port.write_data = response_write_byte;
            response_tail = (response_tail + 1) % config.response_fifo_capacity;
            ++response_size;
            response_write_pending = false;
        }
    }
    std::uint8_t status_byte(unsigned address) {
        if (address == 1) {
            latched_status = status;
            status_latched = true;
            auto& value = latched_status[1];
            value &= 0x0f;
            if (fault != HostFault::None) value |= 0x80;
            if (receive_size == config.rx_fifo_capacity) value |= 0x40;
            if (command_size == config.command_fifo_capacity) value |= 0x20;
            if (total_response_size() != 0) value |= 0x10;
        }
        return status_latched ? latched_status[address] : status[address];
    }
};

HostLink::HostLink(HostLinkConfig config) : impl_(std::make_unique<Impl>(config)) {
    if (config.clock_divider == 0 || config.transfer_phase >= config.clock_divider ||
        config.rx_fifo_capacity == 0 || config.rx_fifo_capacity > 256 ||
        config.command_fifo_capacity == 0 || config.command_fifo_capacity > 2 ||
        config.response_fifo_capacity == 0 || config.response_fifo_capacity > 1024 ||
        config.setup_cycles >= config.clock_divider ||
        config.hold_cycles >= config.clock_divider) {
        throw std::invalid_argument("host link clock/phase/timing or bounded FIFO capacity is invalid");
    }
}
HostLink::~HostLink() = default;
HostLink::HostLink(HostLink&&) noexcept = default;
HostLink& HostLink::operator=(HostLink&&) noexcept = default;

HostBusResult HostLink::tick(const HostPins& pins) {
    auto& p = *impl_;
    ++p.counters.cycles;
    p.start_ports();
    p.abort_requested = p.reset_requested = false;
    HostBusResult result;
    result.transfer_edge = (p.counters.cycles - 1) % p.config.clock_divider == p.config.transfer_phase;
    if (result.transfer_edge) ++p.counters.transfer_edges;

    if (pins.reset || pins.abort) {
        if (pins.reset && !p.previous.reset) {
            p.reset_requested = true;
            ++p.counters.resets;
            p.flush();
            p.fault = HostFault::RecoveryPending;
        } else if (!pins.reset && pins.abort && !p.previous.abort) {
            p.abort_requested = true;
            ++p.counters.aborts;
            p.flush();
            p.fault = HostFault::RecoveryPending;
        }
        p.previous = pins;
        const BsramCycle reset_output{false, 0, 0, false, 0, true};
        p.receive_port = p.response_port = reset_output;
        p.packet_ports.fill(reset_output);
        p.clock_ports();
        return result;
    }

    if (pins.write_valid && pins.read) {
        ++p.counters.bus_direction_violations;
        p.fail(HostFault::BusDirection);
    }
    if (p.counters.cycles < p.hold_until &&
        (pins.data != p.held_data || pins.register_address != p.held_register || pins.read)) {
        ++p.counters.hold_violations;
        p.fail(HostFault::Hold);
    }
    if (p.waiting_write && pins.write_valid &&
        (pins.data != p.waiting_data || pins.register_address != p.waiting_register)) {
        ++p.counters.unstable_writes;
        p.fail(HostFault::UnstableWrite);
    }
    if (!pins.write_valid) p.waiting_write = false;
    const bool same_write = pins.write_valid && p.previous.write_valid && !pins.read &&
        !p.previous.read && pins.data == p.previous.data &&
        pins.register_address == p.previous.register_address;
    const bool same_read = pins.read && p.previous.read && !pins.write_valid &&
        !p.previous.write_valid && pins.register_address == p.previous.register_address;
    p.write_stable = same_write ? p.write_stable + (p.write_stable < p.config.clock_divider) : 0;
    p.read_stable = same_read ? p.read_stable + (p.read_stable < p.config.clock_divider) : 0;

    // READY observes the old registered FIFO occupancy. The parser may free a
    // slot on this edge, but a full FIFO cannot accept using that future state.
    result.write_ready = p.fault == HostFault::None && p.receive_size < p.config.rx_fifo_capacity;
    p.decoder_edge();

    if (pins.write_valid && !result.write_ready) {
        ++p.counters.backpressure_cycles;
        if (result.transfer_edge) ++p.counters.backpressure_edges;
    }
    if (result.transfer_edge && pins.write_valid && !pins.read) {
        if (pins.register_address != 0) {
            p.fail(HostFault::RegisterAddress);
        } else if (result.write_ready) {
            if (p.write_stable < p.config.setup_cycles) {
                ++p.counters.setup_violations;
                p.fail(HostFault::Setup);
            } else {
                p.receive_port.write_enable = true;
                p.receive_port.write_address = static_cast<std::uint16_t>(p.receive_tail);
                p.receive_port.write_data = pins.data;
                p.receive_tail = (p.receive_tail + 1) % p.config.rx_fifo_capacity;
                ++p.receive_size;
                ++p.counters.rx_bytes;
                p.counters.rx_high_water = std::max(p.counters.rx_high_water, p.receive_size);
                result.write_accepted = true;
                p.waiting_write = false;
                p.held_data = pins.data;
                p.held_register = pins.register_address;
                p.hold_until = p.counters.cycles + p.config.hold_cycles;
            }
        } else {
            p.waiting_write = true;
            p.waiting_data = pins.data;
            p.waiting_register = pins.register_address;
        }
    }
    if (result.transfer_edge && pins.read && !pins.write_valid) {
        if (pins.register_address >= 128) {
            p.fail(HostFault::RegisterAddress);
        } else if (p.read_stable < p.config.setup_cycles) {
            ++p.counters.setup_violations;
            p.fail(HostFault::Setup);
        } else if (pins.register_address >= 64) {
            result.read_data = p.capabilities[pins.register_address - 64];
            result.read_valid = true;
            ++p.counters.capability_reads;
        } else if (pins.register_address != 0) {
            result.read_data = p.status_byte(pins.register_address);
            result.read_valid = true;
            ++p.counters.status_reads;
        } else if (p.response_prefetched) {
            result.read_data = p.response_prefetch_byte;
            p.response_prefetched = false;
            result.read_valid = true;
            ++p.counters.tx_bytes;
        }
    }
    p.response_edge();
    p.clock_ports();
    p.previous = pins;
    return result;
}

bool HostLink::command_ready() const { return impl_->command_size != 0; }
const HostCommand& HostLink::front_command() const {
    if (!command_ready()) throw std::logic_error("host command FIFO is empty");
    return impl_->packet_banks[impl_->command_banks[impl_->command_head]];
}
void HostLink::pop_command() {
    auto& p = *impl_;
    if (!command_ready()) throw std::logic_error("host command FIFO is empty");
    if (p.payload_request_pending)
        throw std::logic_error("cannot release a command with a staged packet-bank read");
    p.payload_valid = false;
    p.command_head = (p.command_head + 1) % p.config.command_fifo_capacity;
    --p.command_size;
}
bool HostLink::request_payload_byte(unsigned offset) {
    auto& p = *impl_;
    if (!command_ready()) throw std::logic_error("packet read requires a queued command");
    if (offset >= front_command().payload_size)
        throw std::out_of_range("packet byte read exceeds the accepted payload");
    if (p.payload_request_pending) return false;
    p.payload_request_pending = true;
    p.payload_request_bank = p.command_banks[p.command_head];
    p.payload_request_offset = offset;
    return true;
}
bool HostLink::payload_byte_valid() const { return impl_->payload_valid; }
std::uint8_t HostLink::payload_byte() const {
    if (!payload_byte_valid()) throw std::logic_error("packet byte output is not valid");
    return static_cast<std::uint8_t>(impl_->packet_memory[impl_->payload_output_bank].output());
}
unsigned HostLink::payload_byte_offset() const {
    if (!payload_byte_valid()) throw std::logic_error("packet byte output is not valid");
    return impl_->payload_output_offset;
}
void HostLink::set_status(const std::array<std::uint8_t, 64>& snapshot) { impl_->status = snapshot; }
void HostLink::set_capabilities(const std::array<std::uint8_t, 64>& capabilities) {
    auto& p = *impl_;
    if (p.capabilities_configured && p.capabilities != capabilities)
        throw std::logic_error("capability bank is immutable within the configured device");
    p.capabilities = capabilities;
    p.capabilities_configured = true;
}
bool HostLink::push_response_byte(std::uint8_t byte) {
    auto& p = *impl_;
    if (p.response_write_pending || p.total_response_size() == p.config.response_fifo_capacity) return false;
    p.response_write_pending = true;
    p.response_write_byte = byte;
    p.counters.response_high_water = std::max(p.counters.response_high_water, p.total_response_size());
    return true;
}
unsigned HostLink::response_size() const { return impl_->total_response_size(); }
unsigned HostLink::response_free() const {
    return impl_->config.response_fifo_capacity - impl_->total_response_size();
}
unsigned HostLink::rx_size() const { return impl_->receive_size; }
unsigned HostLink::command_size() const { return impl_->command_size; }
bool HostLink::faulted() const { return impl_->fault != HostFault::None; }
HostFault HostLink::fault() const { return impl_->fault; }
bool HostLink::abort_requested() const { return impl_->abort_requested; }
bool HostLink::reset_requested() const { return impl_->reset_requested; }
std::uint32_t HostLink::epoch() const { return impl_->epoch; }
void HostLink::recover(std::uint32_t epoch) {
    if (epoch == 0 || epoch <= impl_->epoch)
        throw std::invalid_argument("host recovery epoch must advance without wrapping");
    auto& p = *impl_;
    p.flush();
    p.epoch = epoch;
    p.last_sequence = 0;
    p.fault = HostFault::None;
    p.abort_requested = p.reset_requested = false;
}
void HostLink::reset() {
    if (impl_->epoch == std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("host epoch exhausted; construct a fresh power-on session");
    ++impl_->counters.resets;
    recover(impl_->epoch + 1);
}
const HostLinkStatistics& HostLink::statistics() const { return impl_->counters; }
const HostLinkConfig& HostLink::config() const { return impl_->config; }

} // namespace nano_gpu::hw
