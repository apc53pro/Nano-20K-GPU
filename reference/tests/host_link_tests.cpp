#include "nano_gpu/hw/host_link.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nano_gpu::hw;

namespace {
unsigned cases = 0, failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function>
void expect_throws(Function&& function, const std::string& message) {
    bool threw = false;
    try { function(); } catch (const std::exception&) { threw = true; }
    expect(threw, message);
}
void run_case(const char* name, const std::function<void()>& test) {
    ++cases;
    try { test(); }
    catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}
void put32(std::vector<std::uint8_t>& bytes, unsigned offset, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte)
        bytes[offset + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
}
std::vector<std::uint8_t> packet(std::uint32_t sequence, Opcode opcode = Opcode::GetCaps,
                                 const std::vector<std::uint8_t>& payload = {}) {
    std::vector<std::uint8_t> bytes(16 + payload.size());
    bytes[0] = 0x4e; bytes[1] = 0x47; bytes[2] = 0x50; bytes[3] = 0x55;
    bytes[4] = 0; bytes[5] = 2;
    bytes[6] = static_cast<std::uint8_t>(opcode);
    bytes[7] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(opcode) >> 8);
    put32(bytes, 8, static_cast<std::uint32_t>(payload.size()));
    put32(bytes, 12, sequence);
    for (std::size_t byte = 0; byte < payload.size(); ++byte) bytes[16 + byte] = payload[byte];
    return bytes;
}
HostLinkConfig fast_config() {
    HostLinkConfig config;
    config.clock_divider = 1;
    config.transfer_phase = config.setup_cycles = config.hold_cycles = 0;
    return config;
}
void spin(HostLink& link, unsigned edges = 4) {
    for (unsigned edge = 0; edge < edges; ++edge) link.tick();
}
void prepare_controls(HostLink& link) {
    const auto& config = link.config();
    while ((config.transfer_phase + config.clock_divider -
            link.statistics().cycles % config.clock_divider) % config.clock_divider < config.setup_cycles)
        link.tick();
}
void send_bytes(HostLink& link, const std::vector<std::uint8_t>& bytes) {
    prepare_controls(link);
    std::size_t sent = 0;
    for (std::uint64_t budget = bytes.size() * link.config().clock_divider + 2048;
         budget != 0 && sent < bytes.size(); --budget) {
        HostPins pins;
        pins.write_valid = true;
        pins.data = bytes[sent];
        const auto result = link.tick(pins);
        if (result.write_accepted) {
            expect(result.transfer_edge, "write accepted outside a derived host edge");
            ++sent;
        }
        if (link.faulted()) break;
    }
    // Malformed packets may fault only after their final header byte is
    // accepted; callers test that error without asking this driver to retry it.
    expect(sent == bytes.size(), "host input did not drain within its bounded budget");
}
HostBusResult read_register(HostLink& link, unsigned address) {
    prepare_controls(link);
    HostPins pins;
    pins.read = true;
    pins.register_address = static_cast<std::uint8_t>(address);
    for (unsigned edge = 0; edge <= 2 * link.config().clock_divider; ++edge) {
        const auto result = link.tick(pins);
        if (result.transfer_edge) return result;
    }
    throw std::runtime_error("derived host read edge never arrived");
}
std::uint8_t read_reply_byte(HostLink& link) {
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const auto result = read_register(link, 0);
        if (result.read_valid) return result.read_data;
    }
    throw std::runtime_error("queued response never reached the synchronous FIFO output");
}
void expect_fault(std::vector<std::uint8_t> bytes, HostFault fault) {
    HostLink link(fast_config());
    send_bytes(link, bytes);
    spin(link, 20);
    expect(link.faulted() && link.fault() == fault, "wrong parser fault reason");
    expect(!link.command_ready(), "malformed request became executable");
    const auto observed = read_register(link, 1);
    expect(observed.read_valid && (observed.read_data & 0x80) != 0,
           "parser fault blocked out-of-band status access");
}

void transfer_tests() {
    run_case("default eight-bit link accepts one byte per four core edges", [] {
        HostLink link;
        const auto bytes = packet(1);
        std::size_t sent = 0;
        std::uint64_t previous = 0;
        while (sent < bytes.size()) {
            HostPins pins;
            pins.write_valid = true;
            pins.data = bytes[sent];
            const auto result = link.tick(pins);
            if (result.write_accepted) {
                expect(result.transfer_edge && link.statistics().cycles % 4 == 0,
                       "byte transfer is not aligned to the selected host-clock phase");
                if (previous != 0) expect(link.statistics().cycles - previous == 4,
                                          "host bandwidth exceeded one byte per host edge");
                previous = link.statistics().cycles;
                ++sent;
            }
        }
        expect(!link.command_ready(), "newly arrived header bypassed receiver/parser registers");
        link.tick();
        expect(!link.command_ready(), "packet published on the header decoding edge");
        link.tick();
        expect(!link.command_ready(), "packet bypassed the registered receiver read output");
        link.tick();
        expect(link.command_ready() && link.front_command().sequence == 1,
               "complete packet did not cross registered publication boundary");
        expect(link.statistics().rx_bytes == 16 && link.statistics().parser_bytes == 16 &&
               link.statistics().setup_violations == 0 && link.statistics().hold_violations == 0,
               "valid pin timing was not preserved");
    });
    run_case("partial maximum payload cannot execute and complete payload preserves bytes", [] {
        HostLink link;
        std::vector<std::uint8_t> payload(host_max_payload_bytes);
        for (unsigned byte = 0; byte < payload.size(); ++byte)
            payload[byte] = static_cast<std::uint8_t>(byte * 73 + 29);
        const auto bytes = packet(19, Opcode::WriteHeap, payload);
        send_bytes(link, {bytes.begin(), bytes.end() - 1});
        spin(link, 50);
        expect(!link.command_ready() && !link.faulted(), "partial payload executed or faulted");
        send_bytes(link, {bytes.back()});
        spin(link);
        const auto& command = link.front_command();
        expect(command.opcode == Opcode::WriteHeap && command.sequence == 19 &&
               command.payload_size == payload.size(), "decoded metadata changed");
        for (unsigned byte = 0; byte < payload.size(); ++byte)
            expect(command.payload[byte] == payload[byte], "packet payload was dropped or reordered");
        expect(!link.payload_byte_valid(), "unrequested packet RAM output was marked valid");
        for (unsigned byte = 0; byte < payload.size(); ++byte) {
            expect(link.request_payload_byte(byte), "packet read port refused an idle edge");
            expect(!link.request_payload_byte(byte), "packet bank accepted two read addresses on one edge");
            if (byte == 0) expect(!link.payload_byte_valid(), "packet RAM output changed before its edge");
            link.tick();
            expect(link.payload_byte_valid() && link.payload_byte_offset() == byte &&
                   link.payload_byte() == payload[byte], "clocked packet RAM port changed payload data");
        }
        link.tick();
        expect(!link.payload_byte_valid(), "packet read validity leaked across an unrequested edge");
    });
    run_case("bounded command and receive FIFOs apply lossless backpressure", [] {
        auto config = fast_config();
        config.rx_fifo_capacity = 8;
        config.command_fifo_capacity = 1;
        HostLink link(config);
        std::vector<std::uint8_t> program;
        for (unsigned sequence = 1; sequence <= 8; ++sequence) {
            const auto bytes = packet(sequence, Opcode::Fence, {static_cast<std::uint8_t>(sequence)});
            program.insert(program.end(), bytes.begin(), bytes.end());
        }
        std::size_t sent = 0;
        bool blocked = false;
        for (unsigned edge = 0; edge < 1000 && !blocked; ++edge) {
            HostPins pins;
            pins.write_valid = true; pins.data = program[sent];
            const auto result = link.tick(pins);
            if (result.write_accepted) ++sent;
            else blocked = !result.write_ready;
        }
        expect(blocked && link.rx_size() == config.rx_fifo_capacity && link.command_size() == 1,
               "bounded queues did not stop the host when execution was stalled");
        const auto status = read_register(link, 1);
        expect(status.read_valid && (status.read_data & 0x60) == 0x60,
               "full input queues blocked independent status readback");
        unsigned retired = 0;
        for (unsigned edge = 0; edge < 10000 && retired < 8; ++edge) {
            if (link.command_ready()) {
                const auto& command = link.front_command();
                expect(command.sequence == retired + 1 && command.payload[0] == retired + 1,
                       "backpressure lost, duplicated or reordered an accepted packet");
                link.pop_command(); ++retired;
            }
            HostPins pins;
            pins.write_valid = sent < program.size();
            if (pins.write_valid) pins.data = program[sent];
            if (link.tick(pins).write_accepted) ++sent;
        }
        expect(retired == 8 && sent == program.size() && !link.faulted(),
               "bounded stream failed to make progress after execution resumed");
        expect(link.statistics().rx_high_water == 8 && link.statistics().command_high_water == 1 &&
               link.statistics().backpressure_edges != 0 && link.statistics().parser_stall_cycles != 0,
               "queue/stall telemetry does not reflect actual backpressure");
    });
    run_case("reply FIFO backpressures without overwriting unread bytes", [] {
        auto config = fast_config(); config.response_fifo_capacity = 2;
        HostLink link(config);
        expect(link.push_response_byte(0x12) && !link.push_response_byte(0xa7),
               "producer accepted two bytes for one physical write-port edge");
        HostPins pins; pins.read = true;
        expect(!link.tick(pins).read_valid, "new producer byte bypassed the registered FIFO write/read path");
        expect(link.push_response_byte(0xa7), "next producer edge refused an available FIFO slot");
        link.tick();
        expect(!link.push_response_byte(0xee), "reply overflow was not backpressured");
        expect(read_reply_byte(link) == 0x12 && read_reply_byte(link) == 0xa7,
               "reply FIFO overwrote or reordered bytes");
        expect(!read_register(link, 0).read_valid && link.response_size() == 0 && link.response_free() == 2,
               "empty reply FIFO fabricated a response");
    });
    run_case("register-one status snapshot remains coherent across caller updates", [] {
        HostLink link(fast_config());
        std::array<std::uint8_t, 64> status{};
        status[1] = 5; status[2] = 0xfe; status[3] = 0xab;
        link.set_status(status);
        expect(read_register(link, 1).read_data == 5, "status flags changed caller-owned low bits");
        status[1] = 9; status[2] = 0x11; status[3] = 0x22;
        link.set_status(status);
        expect(read_register(link, 2).read_data == 0xfe && read_register(link, 3).read_data == 0xab,
               "multi-byte status snapshot tore across device updates");
        expect(read_register(link, 1).read_data == 9 && read_register(link, 2).read_data == 0x11,
               "new snapshot request did not latch current device status");
    });
    run_case("capability sideband bypasses full command queues and link faults", [] {
        auto config = fast_config(); config.command_fifo_capacity = 1; config.rx_fifo_capacity = 4;
        HostLink link(config);
        std::array<std::uint8_t, 64> capabilities{};
        for (unsigned byte = 0; byte < capabilities.size(); ++byte)
            capabilities[byte] = static_cast<std::uint8_t>(0xa5 ^ (byte * 31));
        link.set_capabilities(capabilities);
        link.set_capabilities(capabilities);
        auto changed = capabilities; changed[0] ^= 1;
        expect_throws([&] { link.set_capabilities(changed); }, "live capability bank changed configuration");
        std::vector<std::uint8_t> program;
        for (unsigned sequence = 1; sequence <= 5; ++sequence) {
            const auto bytes = packet(sequence); program.insert(program.end(), bytes.begin(), bytes.end());
        }
        std::size_t sent = 0;
        bool blocked = false;
        for (unsigned edge = 0; edge < 1000 && !blocked; ++edge) {
            HostPins pins; pins.write_valid = true; pins.data = program[sent];
            const auto result = link.tick(pins);
            if (result.write_accepted) ++sent;
            else blocked = !result.write_ready;
        }
        expect(blocked && link.command_size() == 1 && link.rx_size() == 4,
               "capability test did not actually block the packet execution path");
        for (unsigned byte = 0; byte < capabilities.size(); ++byte) {
            const auto result = read_register(link, 64 + byte);
            expect(result.read_valid && result.read_data == capabilities[byte],
                   "full packet queues blocked or corrupted capability reads");
        }
        expect(!link.faulted() && link.statistics().capability_reads == 64,
               "capability reads used the parser or faulted valid register addresses");
        expect(!read_register(link, 128).read_valid && link.fault() == HostFault::RegisterAddress,
               "reserved sideband address was accepted");
        expect(read_register(link, 64).read_data == capabilities.front() &&
               read_register(link, 127).read_data == capabilities.back(),
               "capability bank was inaccessible after the packet link faulted");
        link.reset();
        expect(read_register(link, 64).read_data == capabilities.front(),
               "session recovery discarded immutable device capabilities");
    });
    run_case("three native packet banks preserve active lease during queued execution stalls", [] {
        auto config = fast_config(); config.rx_fifo_capacity = 8;
        HostLink link(config);
        const std::array<unsigned, 5> lengths{{1024, 257, 1023, 1, 513}};
        std::array<std::vector<std::uint8_t>, 5> payloads;
        std::vector<std::uint8_t> program;
        for (unsigned command = 0; command < payloads.size(); ++command) {
            payloads[command].resize(lengths[command]);
            for (unsigned byte = 0; byte < lengths[command]; ++byte)
                payloads[command][byte] = static_cast<std::uint8_t>((command + 1) * 61 + byte * 37);
            const auto bytes = packet(command + 1, Opcode::WriteHeap, payloads[command]);
            program.insert(program.end(), bytes.begin(), bytes.end());
        }
        std::size_t sent = 0;
        unsigned retired = 0, requested = 0, received = 0;
        bool leased = false;
        for (unsigned edge = 0; edge < 100000 && retired < payloads.size(); ++edge) {
            if (edge >= 4096 && !leased && link.command_ready()) {
                const auto& metadata = link.front_command();
                expect(metadata.sequence == retired + 1 && metadata.payload_size == lengths[retired],
                       "packet-bank ownership changed command metadata");
                requested = received = 0;
                leased = true;
            }
            if (leased && link.payload_byte_valid()) {
                expect(link.payload_byte_offset() == received &&
                       link.payload_byte() == payloads[retired][received],
                       "native packet read observed a staging/queued bank overwrite");
                ++received;
            }
            if (leased && received == lengths[retired]) {
                link.pop_command(); ++retired; leased = false;
            }
            if (leased && requested < lengths[retired] && edge % 5 != 0 && edge % 7 != 0) {
                expect(link.request_payload_byte(requested++), "native packet port unexpectedly busy");
            }
            HostPins pins;
            pins.write_valid = sent < program.size();
            if (pins.write_valid) pins.data = program[sent];
            if (link.tick(pins).write_accepted) ++sent;
        }
        expect(retired == payloads.size() && sent == program.size() && !link.faulted(),
               "stalled packet-bank leases failed to recover and retire all input");
        expect(link.statistics().backpressure_edges != 0 && HostLink::storage_bsram_blocks == 5,
               "test did not exercise bounded native storage under producer backpressure");
    });
    run_case("native reply FIFO preserves ordering across arbitrary ring wraps and stalls", [] {
        auto config = fast_config(); config.response_fifo_capacity = 7;
        HostLink link(config);
        unsigned produced = 0, consumed = 0;
        for (unsigned edge = 0; edge < 10000 && consumed < 500; ++edge) {
            if (produced < 500 && link.push_response_byte(static_cast<std::uint8_t>(produced * 71 + 13))) {
                ++produced;
                expect(!link.push_response_byte(0xee), "producer bypassed single native write port");
            }
            HostPins pins; pins.read = edge % 5 != 0 && edge % 11 != 0;
            const auto result = link.tick(pins);
            if (result.read_valid) {
                expect(result.read_data == static_cast<std::uint8_t>(consumed * 71 + 13),
                       "native reply FIFO lost or reordered a wrapped/stalled byte");
                ++consumed;
            }
            expect(link.response_size() <= config.response_fifo_capacity,
                   "reply pipeline registers escaped the reported physical capacity");
        }
        expect(produced == 500 && consumed == 500 && !link.faulted(),
               "native producer/read-port pipeline did not make bounded forward progress");
    });
}

void parser_tests() {
    run_case("invalid magic remains a framing fault instead of searching payload", [] {
        auto bytes = packet(1); bytes[0] ^= 1;
        expect_fault(bytes, HostFault::Magic);
    });
    run_case("unsupported protocol major and minor are rejected", [] {
        for (unsigned field : {4u, 5u}) {
            auto bytes = packet(1); ++bytes[field];
            expect_fault(bytes, HostFault::Version);
        }
    });
    run_case("obsolete 0.1 packets are rejected instead of executing a legacy path", [] {
        auto bytes = packet(1); bytes[5] = 1;
        expect_fault(bytes, HostFault::Version);
    });
    run_case("unknown opcode and oversized declared payload are rejected", [] {
        for (std::uint16_t code : {std::uint16_t{0}, std::uint16_t{9}, std::uint16_t{65535}}) {
            auto bytes = packet(1); bytes[6] = static_cast<std::uint8_t>(code);
            bytes[7] = static_cast<std::uint8_t>(code >> 8);
            expect_fault(bytes, HostFault::Opcode);
        }
        auto bytes = packet(1); put32(bytes, 8, 1025);
        expect_fault(bytes, HostFault::Length);
    });
    run_case("zero, duplicate and regressing request sequences are rejected", [] {
        expect_fault(packet(0), HostFault::Sequence);
        for (std::uint32_t invalid : {1u, 2u}) {
            HostLink link(fast_config());
            send_bytes(link, packet(2)); spin(link); link.pop_command();
            send_bytes(link, packet(invalid)); spin(link);
            expect(link.fault() == HostFault::Sequence && !link.command_ready(),
                   "request sequence was reused or regressed within an epoch");
        }
    });
    run_case("sequence exhaustion requires recovery rather than silent wrap", [] {
        HostLink link(fast_config());
        send_bytes(link, packet(std::numeric_limits<std::uint32_t>::max())); spin(link);
        link.pop_command();
        send_bytes(link, packet(1)); spin(link);
        expect(link.fault() == HostFault::Sequence, "sequence wrap executed stale session work");
        link.recover(2);
        send_bytes(link, packet(1)); spin(link);
        expect(link.command_ready() && link.front_command().sequence == 1 && link.epoch() == 2,
               "new epoch did not restore a fresh sequence space");
    });
    run_case("out-of-band abort discards partial input and stale replies", [] {
        HostLink link;
        auto bytes = packet(3); bytes.resize(9);
        send_bytes(link, bytes);
        expect(link.push_response_byte(0xde), "could not stage old response");
        HostPins pins; pins.abort = true;
        link.tick(pins);
        expect(link.abort_requested() && link.fault() == HostFault::RecoveryPending &&
               link.rx_size() == 0 && link.response_size() == 0 && !link.command_ready(),
               "abort left partial/queued old-epoch state live");
        link.tick(pins);
        expect(!link.abort_requested(), "held recovery pin retriggered every core edge");
        link.recover(2);
        link.tick(pins);
        expect(!link.abort_requested(), "recovery reinterpreted a held abort pin as a new edge");
        link.tick();
        send_bytes(link, packet(1)); spin(link);
        expect(link.command_ready() && link.front_command().sequence == 1 && !link.faulted(),
               "new boundary did not replace the discarded partial header");
        expect_throws([&] { link.recover(2); }, "recovery reused an epoch");
        expect_throws([&] { link.recover(0); }, "zero epoch was accepted");
    });
}

void timing_tests() {
    run_case("setup violation refuses a byte first driven on its sampling edge", [] {
        HostLink link;
        spin(link, 3);
        HostPins pins; pins.write_valid = true; pins.data = 0x4e;
        const auto result = link.tick(pins);
        expect(!result.write_accepted && link.fault() == HostFault::Setup &&
               link.statistics().setup_violations == 1,
               "zero-setup data was accepted as a valid bus byte");
    });
    run_case("hold violation diagnoses early data change after capture", [] {
        HostLinkConfig config; config.hold_cycles = 2;
        HostLink link(config);
        HostPins pins; pins.write_valid = true; pins.data = 0x4e;
        while (!link.tick(pins).write_accepted) {}
        pins.data = 0x47;
        link.tick(pins);
        expect(link.fault() == HostFault::Hold && link.statistics().hold_violations == 1,
               "captured write ignored the configured hold interval");
    });
    run_case("continuous valid cannot change a backpressured byte", [] {
        auto config = fast_config(); config.rx_fifo_capacity = 1; config.command_fifo_capacity = 1;
        HostLink link(config);
        std::vector<std::uint8_t> bytes;
        for (unsigned sequence = 1; sequence <= 4; ++sequence) {
            const auto next = packet(sequence); bytes.insert(bytes.end(), next.begin(), next.end());
        }
        std::size_t sent = 0;
        HostPins pins;
        for (unsigned budget = 1000; budget != 0; --budget) {
            pins.write_valid = true; pins.data = bytes[sent];
            const auto result = link.tick(pins);
            if (result.write_accepted) ++sent;
            else if (!result.write_ready) break;
        }
        expect(link.statistics().backpressure_edges != 0, "test did not reach backpressure");
        pins.data ^= 1;
        link.tick(pins);
        expect(link.fault() == HostFault::UnstableWrite && link.statistics().unstable_writes == 1,
               "changed pending bus byte escaped timing diagnostics");
    });
    run_case("shared-bus direction conflict does not transfer data", [] {
        HostLink link(fast_config());
        link.push_response_byte(0x77);
        HostPins pins; pins.write_valid = pins.read = true; pins.data = 0x4e;
        const auto result = link.tick(pins);
        expect(link.fault() == HostFault::BusDirection && !result.write_accepted && !result.read_valid &&
               link.response_size() == 1, "simultaneous bus drivers caused a data transfer");
        expect((read_register(link, 1).read_data & 0x80) != 0,
               "bus fault made status inaccessible");
    });
    run_case("invalid physical configuration and reserved register writes fail", [] {
        for (unsigned capacity : {0u, 257u}) {
            HostLinkConfig config; config.rx_fifo_capacity = capacity;
            expect_throws([&] { HostLink link(config); }, "invalid physical receive capacity accepted");
        }
        HostLink link(fast_config());
        HostPins pins; pins.write_valid = true; pins.register_address = 1;
        link.tick(pins);
        expect(link.fault() == HostFault::RegisterAddress, "status register accepted an input byte");
        link.reset();
        expect(link.epoch() == 2 && !link.faulted(), "explicit link reset did not create a fresh session");
    });
}
} // namespace

int main() {
    transfer_tests(); parser_tests(); timing_tests();
    std::cout << cases - failures << '/' << cases << " host link cases passed\n";
    return failures == 0 ? 0 : 1;
}
