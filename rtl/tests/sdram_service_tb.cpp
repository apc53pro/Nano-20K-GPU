// Physical-pin regression for nano_sdram_service. GPU contents can change only
// through real SDRAM commands and DQ/DQM sampled by SdramPinsModel.
#include "Vnano_sdram_service.h"
#include "verilated.h"
#include "sdram_model.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using nano_gpu::rtl::SdramPinsModel;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Packed> void put(Packed& words, unsigned offset, unsigned width, std::uint32_t value) {
    for (unsigned bit = 0; bit < width; ++bit) {
        const auto mask = std::uint32_t{1} << ((offset + bit) & 31);
        auto& word = words[(offset + bit) >> 5];
        word = (word & ~mask) | (((value >> bit) & 1u) ? mask : 0u);
    }
}

struct Events {
    unsigned commands = 0, writes = 0, reads = 0, last = 0, done = 0, error = 0;
    std::array<std::uint32_t, 6> data{};
};

struct Test {
    VerilatedContext context;
    Vnano_sdram_service dut{&context};
    SdramPinsModel chip;
    std::uint64_t edges = 0;
    unsigned transactions = 0;

    Test() {
        dut.clk = 0; dut.reset = 1;
        dut.cmd_valid = dut.cmd_write = 0; dut.cmd_words = 0;
        dut.w_valid = 0; dut.w_strb = 0; dut.r_ready = 63;
        for (unsigned i = 0; i < 5; ++i) dut.cmd_addr[i] = 0;
        for (unsigned i = 0; i < 6; ++i) dut.w_data[i] = 0;
        dut.sd_dq_in = 0;
        for (unsigned i = 0; i < 4; ++i) step();
        dut.reset = 0;
        while (!dut.initialized) step();
        require(edges >= 10080, "controller bypassed its physical power-up interval");
        require(chip.statistics().refreshes == 2, "initialization omitted two AUTO REFRESH commands");
    }

    Events step() {
        if (edges > 1000000) throw std::runtime_error("SDRAM regression timed out");
        dut.clk = 0; dut.eval();
        Events event;
        event.commands = dut.cmd_valid & dut.cmd_ready;
        event.writes = dut.w_valid & dut.w_ready;
        event.reads = dut.r_valid & dut.r_ready;
        event.last = dut.r_last;
        for (unsigned i = 0; i < 6; ++i) event.data[i] = dut.r_data[i];
        SdramPinsModel::Pins pins;
        pins.cke = dut.sd_cke; pins.cs_n = dut.sd_cs_n;
        pins.ras_n = dut.sd_ras_n; pins.cas_n = dut.sd_cas_n; pins.we_n = dut.sd_we_n;
        pins.bank = dut.sd_ba; pins.address = dut.sd_addr;
        pins.dq_oe = dut.sd_dq_oe; pins.dq_out = dut.sd_dq_out; pins.dqm = dut.sd_dqm;
        // Both sides sample OLD outputs. The chip's new DQ is assigned only
        // after the RTL edge and can first be captured on the following edge.
        // Advance simulator time too: the native RAM collision checker must
        // distinguish this edge from earlier cycles at the same address.
        context.timeInc(5000); // half-cycle in the design's 1 ps precision
        dut.clk = 1; dut.eval();
        dut.sd_dq_in = chip.rising_edge(pins); dut.eval();
        event.done = dut.done; event.error = dut.error;
        context.timeInc(5000);
        dut.clk = 0; dut.eval(); ++edges;
        return event;
    }

    void idle(unsigned count) { for (unsigned i = 0; i < count; ++i) step(); }

    Events command(unsigned client, bool write, std::uint32_t address, unsigned words) {
        const auto mask = 1u << client;
        put(dut.cmd_addr, client * 23, 23, address);
        dut.cmd_words = (dut.cmd_words & ~(31u << (client * 5))) | ((words & 31u) << (client * 5));
        dut.cmd_write = (dut.cmd_write & ~mask) | (write ? mask : 0);
        dut.cmd_valid |= mask;
        Events event;
        do event = step(); while (!(event.commands & mask));
        dut.cmd_valid &= ~mask; ++transactions;
        return event;
    }

    void write(unsigned client, std::uint32_t address, const std::vector<std::uint32_t>& data,
               const std::vector<unsigned>& masks, bool long_stall = false) {
        const unsigned bit = 1u << client;
        auto event = command(client, true, address, data.size());
        require((event.error & bit) == 0, "valid write command rejected");
        if (long_stall) {
            const auto refreshes = chip.statistics().refreshes;
            idle(4000);
            require(chip.statistics().refreshes > refreshes + 1, "producer backpressure blocked refresh");
        }
        for (unsigned index = 0; index < data.size(); ++index) {
            idle(index % 3);
            dut.w_data[client] = data[index];
            dut.w_strb = (dut.w_strb & ~(15u << (client * 4))) | (masks[index] << (client * 4));
            dut.w_valid |= bit;
            do event = step(); while (!(event.writes & bit));
            dut.w_valid &= ~bit;
        }
        do event = step(); while (!(event.done & bit));
        require((event.error & bit) == 0, "write response failed");
    }

    std::vector<std::uint32_t> read(unsigned client, std::uint32_t address, unsigned words, bool long_stall = false) {
        const unsigned bit = 1u << client;
        auto event = command(client, false, address, words);
        require((event.error & bit) == 0, "valid read command rejected");
        dut.r_ready &= ~bit;
        if (long_stall) {
            while (!(dut.r_valid & bit)) step();
            const auto retained = dut.r_data[client];
            const auto refreshes = chip.statistics().refreshes;
            for (unsigned i = 0; i < 4000; ++i) {
                step();
                require((dut.r_valid & bit) && dut.r_data[client] == retained,
                        "read response changed during consumer backpressure/refresh");
            }
            require(chip.statistics().refreshes > refreshes + 1, "consumer backpressure blocked refresh");
        }
        std::vector<std::uint32_t> result;
        bool completed = false;
        while (!completed) {
            if ((edges & 3u) == 0) dut.r_ready &= ~bit;
            else dut.r_ready |= bit;
            event = step();
            if (event.reads & bit) {
                result.push_back(event.data[client]);
                require(bool(event.last & bit) == (result.size() == words), "read last-word marker misplaced");
            }
            require((event.error & bit) == 0, "read response failed");
            completed = (event.done & bit) != 0;
        }
        dut.r_ready |= bit;
        require(result.size() == words, "read completed before every response handshake");
        return result;
    }
};
} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        Test test;
        // Exhaust every BL8 prefix and requested length, including the case
        // requiring three physical bursts for sixteen useful words.
        for (unsigned prefix = 0; prefix < 8; ++prefix) for (unsigned length = 1; length <= 16; ++length) {
            const std::uint32_t address = 0x10000 + prefix * 4;
            std::vector<std::uint32_t> words, expected;
            std::vector<unsigned> masks;
            for (unsigned i = 0; i < length; ++i) {
                words.push_back(0x12345678u ^ (prefix * 0x1337u) ^ (length << 20) ^ (i * 0x1020304u));
                masks.push_back((i + prefix + length) & 15u);
                auto value = test.chip.inspect_word(address + 4 * i);
                for (unsigned byte = 0; byte < 4; ++byte) if (masks.back() & (1u << byte)) {
                    const auto mask = 255u << (8 * byte);
                    value = (value & ~mask) | (words.back() & mask);
                }
                expected.push_back(value);
            }
            const auto before = test.chip.inspect_word(address - 4);
            const auto after = test.chip.inspect_word(address + 4 * length);
            test.write((prefix + length) % 6, address, words, masks);
            require(test.chip.inspect_word(address - 4) == before &&
                    test.chip.inspect_word(address + 4 * length) == after,
                    "masked BL8 padding overwrote a neighboring word");
            require(test.read((prefix + length + 1) % 6, address, length) == expected,
                    "physical BL8 read/write data mismatch");
        }
        // Column/bank/row and the final physical allocation boundary.
        for (const auto address : {0x3fcu, 0xffcu, 0x7fffc0u, 0x7ffffcu}) {
            const unsigned length = address == 0x7ffffcu ? 1 : 16;
            std::vector<std::uint32_t> words(length);
            for (unsigned i = 0; i < length; ++i) words[i] = address ^ (0x9e3779b9u * (i + 1));
            test.write(3, address, words, std::vector<unsigned>(length, 15));
            require(test.read(4, address, length) == words, "physical bank/row/boundary crossing mismatch");
        }
        std::vector<std::uint32_t> stalled(16, 0x76543210u);
        test.write(1, 0x50000, stalled, std::vector<unsigned>(16, 15), true);
        require(test.read(2, 0x50000, 16, true) == stalled, "refresh damaged stalled request/response contents");

        for (const auto item : std::array<std::array<unsigned, 2>, 4>{{
                {{0x101, 1}}, {{0x100, 0}}, {{0x100, 17}}, {{0x7ffffc, 2}}}}) {
            auto event = test.command(5, false, item[0], item[1]);
            require(!(event.done & 32u) && !(event.error & 32u), "registered validation completed before its edge");
            do event = test.step(); while (!(event.done & 32u));
            require(event.error & 32u, "invalid memory command did not complete with error");
        }

        // Continuous scanout demand cannot starve the five other clients.
        for (unsigned client = 0; client < 6; ++client) {
            put(test.dut.cmd_addr, client * 23, 23, 0x50000);
            test.dut.cmd_words = (test.dut.cmd_words & ~(31u << (client * 5))) | (1u << (client * 5));
        }
        test.dut.cmd_valid = 63; test.dut.cmd_write = 0; test.dut.r_ready = 63;
        unsigned completed = 0, display_run = 0;
        while ((completed & 62u) != 62u) {
            const auto event = test.step();
            if (event.commands & 1u) ++display_run;
            if (event.commands & 62u) { require(display_run <= 4, "scanout priority starved another client"); display_run = 0; }
            test.dut.cmd_valid &= ~(event.commands & 62u);
            completed |= event.done;
        }
        test.dut.cmd_valid = 0; test.idle(2000);
        require(test.chip.statistics().max_refresh_gap <= 1575, "periodic refresh retention deadline exceeded");
        // Runtime FPGA reset does not reset or power-cycle the SDRAM chip.
        // Reset just before each WRITE phase, retaining the same chip model.
        // Any remainder must be driven with DQM masking, then reinitialized.
        for (unsigned phase = 0; phase < 8; ++phase) {
            Test interrupted;
            interrupted.command(1, true, 0x60000, 16);
            for (unsigned index = 0; index < 16; ++index) {
                interrupted.dut.w_data[1] = 0xdead0000u + index;
                interrupted.dut.w_strb |= 15u << 4;
                interrupted.dut.w_valid = 2;
                Events event;
                do event = interrupted.step(); while (!(event.writes & 2u));
                interrupted.dut.w_valid = 0;
            }
            const auto write_command = [&] {
                return !interrupted.dut.sd_cs_n && interrupted.dut.sd_ras_n &&
                    !interrupted.dut.sd_cas_n && !interrupted.dut.sd_we_n;
            };
            while (!write_command()) interrupted.step();
            for (unsigned index = 0; index < phase; ++index) interrupted.step();
            interrupted.dut.reset = 1;
            interrupted.idle(3); // deliberately shorter than some remaining tails
            interrupted.dut.reset = 0;
            while (!interrupted.dut.initialized) interrupted.step();
            const std::vector<std::uint32_t> replacement(16, 0x24681357u ^ phase);
            interrupted.write(3, 0x60000, replacement, std::vector<unsigned>(16, 15));
            require(interrupted.read(4, 0x60000, 16) == replacement,
                    "runtime reset left a physical burst/owner/bank state wedged");
        }
        std::cout << "SDRAM pin regression passed: " << test.transactions << " requests, "
                  << test.chip.statistics().read_commands << " physical reads, "
                  << test.chip.statistics().write_commands << " physical writes, max refresh gap "
                  << test.chip.statistics().max_refresh_gap << " cycles; all eight runtime WRITE reset phases\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "SDRAM pin regression failed: " << exception.what() << '\n';
        return 1;
    }
}
