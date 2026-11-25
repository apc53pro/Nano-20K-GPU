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
constexpr unsigned depth_client = 3, depth_bit = 1u << depth_client;
constexpr std::uint32_t depth_begin = 0x12c000, depth_end = 0x258000;
constexpr unsigned depth_words = 32, depth_token = 16;
constexpr unsigned depth_bytes = depth_words * 4;

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
        dut.cmd_valid = dut.cmd_write = 0; dut.cmd_words = 0; dut.cmd_depth_row32 = 0;
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

    Events command(unsigned client, bool write, std::uint32_t address, unsigned words, bool depth_row32 = false) {
        const auto mask = 1u << client;
        put(dut.cmd_addr, client * 23, 23, address);
        dut.cmd_words = (dut.cmd_words & ~(31u << (client * 5))) | ((words & 31u) << (client * 5));
        dut.cmd_write = (dut.cmd_write & ~mask) | (write ? mask : 0);
        dut.cmd_depth_row32 = depth_row32; dut.cmd_valid |= mask;
        Events event;
        do event = step(); while (!(event.commands & mask));
        dut.cmd_valid &= ~mask; dut.cmd_depth_row32 = 0; ++transactions;
        return event;
    }

    void write_payload(unsigned client, std::uint32_t data, unsigned mask) {
        const unsigned bit = 1u << client;
        dut.w_data[client] = data;
        dut.w_strb = (dut.w_strb & ~(15u << (client * 4))) | (mask << (client * 4));
        dut.w_valid |= bit;
        Events event;
        do {
            event = step();
            require(((event.done | event.error) & bit) == 0, "write completed before payload");
        } while (!(event.writes & bit));
        dut.w_valid &= ~bit;
    }

    void write(unsigned client, std::uint32_t address, const std::vector<std::uint32_t>& data,
               const std::vector<unsigned>& masks, bool long_stall = false, bool depth_row32 = false) {
        const unsigned bit = 1u << client;
        auto event = command(client, true, address, depth_row32 ? depth_token : data.size(), depth_row32);
        require((event.error & bit) == 0, "valid write command rejected");
        if (long_stall) {
            const auto refreshes = chip.statistics().refreshes;
            idle(4000);
            require(chip.statistics().refreshes > refreshes + 1, "producer backpressure blocked refresh");
        }
        for (unsigned index = 0; index < data.size(); ++index) {
            idle(index % 3);
            write_payload(client, data[index], masks[index]);
        }
        do event = step(); while (!(event.done & bit));
        require((event.error & bit) == 0, "write response failed");
    }

    std::vector<std::uint32_t> read(unsigned client, std::uint32_t address, unsigned words,
                                    bool long_stall = false, bool depth_row32 = false) {
        const unsigned bit = 1u << client;
        auto event = command(client, false, address, depth_row32 ? depth_token : words, depth_row32);
        require((event.error & bit) == 0, "valid read command rejected");
        dut.r_ready &= ~bit;
        if (long_stall) {
            while (!(dut.r_valid & bit)) step();
            const auto retained = dut.r_data[client];
            const auto refreshes = chip.statistics().refreshes;
            for (unsigned i = 0; i < 4000; ++i) {
                event = step();
                require((dut.r_valid & bit) && dut.r_data[client] == retained && bool(dut.r_last & bit) == (words == 1) &&
                        !((event.done | event.error) & bit),
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

std::vector<std::uint32_t> exercise_depth_row(Test& test, std::uint32_t address) {
    std::vector<std::uint32_t> initial(depth_words), data(depth_words), wanted(depth_words);
    std::vector<unsigned> masks(depth_words);
    for (unsigned i = 0; i < depth_words; ++i) {
        initial[i] = (address ^ 0x13579bd0u) + 0x01020304u * i;
        data[i] = 0xfedcba98u ^ (0x9e3779b9u * (i + 1));
        masks[i] = (i * 5 + 1) & 15u;
        wanted[i] = initial[i];
        for (unsigned byte = 0; byte < 4; ++byte) if (masks[i] & (1u << byte)) {
            const auto byte_mask = 255u << (8 * byte);
            wanted[i] = (wanted[i] & ~byte_mask) | (data[i] & byte_mask);
        }
    }
    const std::vector<unsigned> full(depth_token, 15);
    test.write(1, address - 4, {0x9728f182u}, {15});
    test.write(1, address + depth_bytes, {0xeaf03847u}, {15});
    test.write(1, address, {initial.begin(), initial.begin() + depth_token}, full);
    test.write(1, address + 64, {initial.begin() + depth_token, initial.end()}, full);
    const auto before_write = test.chip.statistics();
    test.write(depth_client, address, data, masks, address == depth_begin, true);
    const auto after_write = test.chip.statistics();
    require(after_write.write_commands - before_write.write_commands == 4 &&
            after_write.write_words - before_write.write_words == depth_words,
            "depth row WRITE did not issue four BL8s");
    for (unsigned i = 0; i < depth_words; ++i)
        require(test.chip.inspect_word(address + 4 * i) == wanted[i],
                "depth row WRITE altered payload order or byte masks");
    require(test.chip.inspect_word(address - 4) == 0x9728f182u &&
            test.chip.inspect_word(address + depth_bytes) == 0xeaf03847u,
            "depth row WRITE touched a neighboring word");
    const auto before_read = test.chip.statistics();
    require(test.read(depth_client, address, depth_words, address != depth_begin, true) == wanted,
            "depth row READ data, order or completion mismatch");
    const auto after_read = test.chip.statistics();
    require(after_read.read_commands - before_read.read_commands == 4 &&
            after_read.read_words - before_read.read_words == depth_words,
            "depth row READ did not issue four BL8s");
    return wanted;
}

void exercise_mixed_slots(Test& test, const std::vector<std::uint32_t>& expected_depth) {
    constexpr unsigned copy_client = 5, copy_bit = 1u << copy_client;
    constexpr unsigned scan_client = 0, scan_bit = 1u << scan_client;
    constexpr std::uint32_t copy_address = 0x360000, scan_address = depth_begin + 64;
    constexpr std::uint32_t scan_anchor = 0x3468129bu;
    const auto before = test.chip.statistics();
    test.dut.r_ready &= ~depth_bit;
    test.command(depth_client, false, depth_begin, depth_token, true);
    while (!(test.dut.r_valid & depth_bit)) test.step();
    const auto retained = test.dut.r_data[depth_client];
    // A normal-width write may be accepted into the other slot while the
    // entire depth read response is still outstanding. A third request must
    // wait; the shared flag is high when client 5 is selected.
    auto event = test.command(copy_client, true, copy_address, depth_token, true);
    require((event.done | event.error) == 0 && (test.dut.r_valid & depth_bit),
            "ordinary write did not occupy the free slot behind a held depth read");
    for (unsigned i = 0; i < depth_token; ++i)
        test.write_payload(copy_client, scan_anchor ^ (0x2735b9a1u * i), 15);
    put(test.dut.cmd_addr, scan_client * 23, 23, scan_address);
    test.dut.cmd_words = (test.dut.cmd_words & ~31u) | 1u;
    test.dut.cmd_write &= ~scan_bit; test.dut.cmd_valid |= scan_bit;
    const auto refreshes = test.chip.statistics().refreshes;
    for (unsigned i = 0; i < 4000; ++i) {
        event = test.step();
        require(!(event.commands | event.done | event.error) &&
                (test.dut.r_valid & depth_bit) && !(test.dut.r_last & depth_bit) &&
                test.dut.r_data[depth_client] == retained,
                "two full slots lost a held response or admitted a third command");
    }
    require(test.chip.statistics().refreshes > refreshes + 1 &&
            test.chip.statistics().write_commands == before.write_commands,
            "mixed-slot hold blocked refresh or allowed a WRITE before the READ response");
    test.dut.r_ready |= depth_bit;
    unsigned depth_index = 0, scan_reads = 0, completed = 0, scan_commands = 0;
    while (completed != (depth_bit | copy_bit | scan_bit)) {
        event = test.step();
        if (event.commands & scan_bit) {
            test.dut.cmd_valid &= ~scan_bit; ++test.transactions; ++scan_commands;
            require(depth_index == depth_words, "third request overtook the occupied depth-response slot");
        }
        if (event.reads & depth_bit) {
            require(depth_index < depth_words && event.data[depth_client] == expected_depth[depth_index],
                    "concurrent request corrupted depth-row data");
            ++depth_index;
            require(bool(event.last & depth_bit) == (depth_index == depth_words), "mixed depth READ last was misplaced");
        }
        if (event.reads & scan_bit) {
            require(++scan_reads == 1 && (event.last & scan_bit) &&
                    event.data[scan_client] == expected_depth[depth_token],
                    "mixed-slot younger scanout READ returned the wrong data");
        }
        require(!event.error && !(completed & event.done), "mixed-slot completion duplicated or faulted");
        completed |= event.done;
        if ((completed & depth_bit) != 0) require(depth_index == depth_words, "depth READ completed early");
        if ((completed & scan_bit) != 0) require(scan_reads == 1, "scanout READ completed early");
    }
    require(scan_commands == 1 && scan_reads == 1 && depth_index == depth_words &&
            test.chip.statistics().write_commands - before.write_commands == 2,
            "mixed-slot ordinary write did not remain sixteen words");
    for (unsigned i = 0; i < depth_token; ++i)
        require(test.chip.inspect_word(copy_address + 4 * i) == (scan_anchor ^ (0x2735b9a1u * i)),
                "mixed-slot ordinary payload was lost");
}

void expect_invalid_depth(Test& test, unsigned client, bool write, std::uint32_t address, unsigned raw_words) {
    const auto bit = 1u << client;
    const auto before = test.chip.statistics();
    auto event = test.command(client, write, address, raw_words, true);
    require(!event.done && !event.error, "invalid depth command bypassed retained validation");
    // Offer payload even for malformed writes. Rejection must happen before
    // DMA consumes it or emits a READ/WRITE to the chip.
    test.dut.w_valid |= bit;
    do {
        event = test.step();
        require(!event.reads && !event.writes, "invalid depth command transferred payload");
    } while (!event.done);
    test.dut.w_valid &= ~bit;
    require(event.done == bit && event.error == bit, "invalid depth command errored for the wrong owner");
    event = test.step();
    require(!event.done && !event.error, "invalid depth completion repeated");
    const auto after = test.chip.statistics();
    require(after.read_commands == before.read_commands && after.write_commands == before.write_commands,
            "invalid depth command reached SDRAM pins");
}
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

        const auto first_depth = exercise_depth_row(test, depth_begin);
        exercise_depth_row(test, depth_end - depth_bytes);
        exercise_mixed_slots(test, first_depth);
        unsigned rejected = 0, ordinary = 1; // The mixed-slot COPY was selected with the flag high.
        for (unsigned client = 0; client < 6; ++client) if (client != depth_client) {
            // The scalar flag can be high while a different client wins.
            // Its valid raw-16 read must complete at exactly sixteen words.
            const auto expected = std::vector<std::uint32_t>(first_depth.begin(),
                                                             first_depth.begin() + depth_token);
            auto event = test.command(client, false, depth_begin, depth_token, true);
            require((event.error & (1u << client)) == 0, "non-depth owner's raw-16 command rejected");
            unsigned words = 0;
            do {
                event = test.step();
                if (event.reads & (1u << client)) {
                    require(words < expected.size() && event.data[client] == expected[words],
                            "private depth flag altered ordinary READ data or count");
                    ++words;
                    require(bool(event.last & (1u << client)) == (words == depth_token),
                            "private depth flag altered ordinary READ last");
                }
                require(!event.error, "private depth flag rejected an ordinary command");
            } while (!(event.done & (1u << client)));
            require(words == depth_token, "private depth flag extended an ordinary command");
            ++ordinary;
            expect_invalid_depth(test, client, client % 2 == 0, depth_begin, 17);
            ++rejected;
        }
        for (unsigned token : {0u, 1u, 15u, 17u, 31u}) {
            expect_invalid_depth(test, depth_client, token % 2 != 0, depth_begin, token);
            ++rejected;
        }
        for (auto address : {depth_begin - depth_bytes, depth_begin + 4, depth_begin + 32,
                             depth_end - 4, depth_end, 0x7fff80u}) {
            expect_invalid_depth(test, depth_client, true, address, depth_token);
            ++rejected;
        }
        std::cout << "PASS depth rows: physical2 bl8_read8 bl8_write8 invalid" << rejected
                  << " ordinary" << ordinary << " mixed1 refresh1\n";

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
