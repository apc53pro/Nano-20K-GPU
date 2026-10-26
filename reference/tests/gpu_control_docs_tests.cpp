// The C99 harness includes code extracted from GPU_DOCUMENTATION/GPU_CONTROL.md.
// This bridge supplies only clocked host pins; it never edits device memory or
// calls SystemModel's convenience submission/rendering methods.
#include "nano_gpu/hw/protocol.hpp"
#include "nano_gpu/hw/system.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
using DocsWrite = bool (*)(void*, std::uint8_t);
using DocsRead = bool (*)(void*, std::uint8_t*);
using DocsRegister = bool (*)(void*, std::uint8_t, std::uint8_t*);
int gpu_docs_mock_tests();
int gpu_docs_model_tests(void*, DocsWrite, DocsRead, DocsRegister);
const char* gpu_docs_test_error();
}

namespace {
using namespace nano_gpu::hw;

struct Bus {
    SystemModel model;
    std::string error;
    static constexpr std::uint64_t timeout = 50000000;

    bool check(std::uint64_t start) {
        if (model.faulted()) {
            error = "device fault: " + model.fault_message();
            return false;
        }
        if (model.system_statistics().core_cycles - start > timeout) {
            error = "bounded host transaction timed out";
            return false;
        }
        return true;
    }
    void align() {
        const auto& config = model.config().host;
        const auto phase = (config.transfer_phase + 1) % config.clock_divider;
        while (model.host_statistics().cycles % config.clock_divider != phase) model.tick();
    }
    void hold() {
        for (unsigned i = 0; i < model.config().host.hold_cycles; ++i) model.tick();
    }
    bool write(std::uint8_t byte) {
        align();
        const auto start = model.system_statistics().core_cycles;
        HostPins pins;
        pins.write_valid = true;
        pins.data = byte;
        for (;;) {
            const auto result = model.tick(pins);
            if (!check(start)) return false;
            if (result.write_accepted) { hold(); return true; }
        }
    }
    bool read(std::uint8_t address, std::uint8_t* byte) {
        align();
        const auto start = model.system_statistics().core_cycles;
        HostPins pins;
        pins.read = true;
        pins.register_address = address;
        for (;;) {
            const auto result = model.tick(pins);
            if (!check(start)) return false;
            if (result.read_valid) { *byte = result.read_data; hold(); return true; }
        }
    }
};

bool write_byte(void* user, std::uint8_t byte) { return static_cast<Bus*>(user)->write(byte); }
bool read_byte(void* user, std::uint8_t* byte) { return static_cast<Bus*>(user)->read(0, byte); }
bool read_register(void* user, std::uint8_t address, std::uint8_t* byte) {
    return static_cast<Bus*>(user)->read(address, byte);
}
} // namespace

extern "C" int gpu_docs_packet_matches(const std::uint8_t* actual, std::size_t actual_size,
    std::uint16_t opcode, std::uint32_t sequence, const std::uint8_t* payload, std::size_t size) {
    try {
        std::vector<std::uint8_t> bytes;
        if (size != 0) bytes.assign(payload, payload + size);
        const auto expected = nano_gpu::hw::encode_packet(static_cast<nano_gpu::hw::Opcode>(opcode), sequence, bytes);
        return expected.size() == actual_size && std::equal(expected.begin(), expected.end(), actual);
    } catch (const std::exception&) { return 0; }
}

int main() {
    try {
        if (!gpu_docs_mock_tests()) {
            std::cerr << "C99 control example mock test failed: " << gpu_docs_test_error() << '\n';
            return 1;
        }
        Bus bus;
        const auto start = bus.model.system_statistics().core_cycles;
        while (!bus.model.ready()) {
            bus.model.tick();
            if (!bus.check(start)) {
                std::cerr << "C99 control example boot failed: " << bus.error << '\n';
                return 1;
            }
        }
        if (!gpu_docs_model_tests(&bus, write_byte, read_byte, read_register)) {
            std::cerr << "C99 control example bus test failed: " << gpu_docs_test_error();
            if (!bus.error.empty()) std::cerr << " (" << bus.error << ')';
            std::cerr << '\n';
            return 1;
        }
        const auto& host = bus.model.host_statistics();
        if (host.rx_bytes == 0 || host.tx_bytes == 0 || host.status_reads == 0 ||
            host.setup_violations || host.hold_violations || host.unstable_writes || host.protocol_faults) {
            std::cerr << "C99 control example did not use a valid clocked host link\n";
            return 1;
        }
        std::cout << "GPU control guide C99 examples passed: canonical packets, bounded failures, "
                     "reply validation, tiled rendering, movement, readback and presentation "
                     "over clocked host pins (" << bus.model.system_statistics().core_cycles << " core cycles).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "C99 control example exception: " << error.what() << '\n';
        return 1;
    }
}
