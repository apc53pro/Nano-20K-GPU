#include "nano_gpu/hw/sdram.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace nano_gpu::hw;

namespace {

unsigned cases = 0;
unsigned failures = 0;

void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

template<class Function>
void throws(Function&& function, const std::string& message) {
    bool did_throw = false;
    try { function(); }
    catch (const std::exception&) { did_throw = true; }
    expect(did_throw, message);
}

template<class Function>
void test(const std::string& name, Function&& function) {
    ++cases;
    try { function(); }
    catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

// An independent pin-level observer reconstructs the device from ACT/PRE/READ/
// WRITE/DQM, not from the controller's requests. It checks physical timing and
// compares every visible read beat with previously observed masked DQ writes.
class PinObserver {
    struct Bank {
        bool open = false;
        unsigned row = 0;
        std::uint64_t active = 0;
        std::uint64_t precharge = 0;
        std::uint64_t last_write = 0;
    };
    std::array<Bank, 4> banks_{};
    std::map<std::uint32_t, std::uint32_t> storage_;
    std::uint64_t refresh_ = 0;
    std::uint64_t mode_ = 0;
    unsigned remaining_ = 0;
    unsigned transferred_ = 0;
    unsigned data_bank_ = 0;
    std::uint32_t burst_word_ = 0;
    std::uint64_t data_at_ = 0;
    bool writing_ = false;
    bool seen_data_ = false;
    bool prior_write_ = false;
    std::uint64_t prior_data_ = 0;
    std::array<std::uint8_t, 2> dqm_history_{{0xf, 0xf}};
public:
    std::vector<std::pair<std::uint64_t, SdramCommand>> commands;

    std::uint32_t value(std::uint32_t byte_address) const {
        const auto found = storage_.find(byte_address / 4);
        return found == storage_.end() ? 0xcdcdcdcdu : found->second;
    }

    void edge(const SdramController& memory) {
        const auto now = memory.cycles();
        const auto& pin = memory.pins();
        const auto& config = memory.config();
        const auto read_dqm = dqm_history_[0];
        dqm_history_[0] = dqm_history_[1];
        dqm_history_[1] = pin.dqm;
        expect(pin.cke, "selected model keeps stable SDR clock enable high");
        if (pin.command != SdramCommand::Nop) {
            expect(now >= config.power_up_cycles, "command issued during mandatory power-up wait");
            commands.emplace_back(now, pin.command);
        }
        auto precharge = [&](Bank& bank) {
            if (bank.open) expect(now >= bank.active + config.t_ras, "pin trace violated tRAS");
            if (bank.last_write != 0) expect(now >= bank.last_write + config.t_wr, "pin trace violated tWR");
            bank.open = false;
            bank.precharge = now;
        };
        switch (pin.command) {
        case SdramCommand::Nop: break;
        case SdramCommand::PrechargeAll:
            expect((pin.address & (1u << 10)) != 0, "PRECHARGE ALL must assert A10");
            for (auto& bank : banks_) precharge(bank);
            break;
        case SdramCommand::Precharge:
            precharge(banks_[pin.bank]);
            break;
        case SdramCommand::AutoRefresh:
            expect(remaining_ == 0, "refresh overlapped DQ burst");
            for (const auto& bank : banks_)
                expect(!bank.open && now >= bank.precharge + config.t_rp,
                       "AUTO REFRESH needs closed banks after tRP");
            if (refresh_ != 0) expect(now >= refresh_ + config.t_rfc, "refresh violated tRFC");
            refresh_ = now;
            break;
        case SdramCommand::LoadMode:
            expect(now >= refresh_ + config.t_rfc, "mode register violated refresh recovery");
            expect((pin.address & 7u) == 3 && ((pin.address >> 4) & 7u) == config.cas_latency,
                   "mode must actually select sequential BL8 and configured CL");
            mode_ = now;
            break;
        case SdramCommand::Active: {
            auto& bank = banks_[pin.bank];
            expect(mode_ != 0 && now >= mode_ + config.t_mrd, "ACTIVE before mode-register recovery");
            expect(!bank.open && now >= bank.precharge + config.t_rp, "ACTIVE violated tRP/open-row state");
            if (bank.active != 0) expect(now >= bank.active + config.t_rc, "ACTIVE violated tRC");
            expect(now >= refresh_ + config.t_rfc, "ACTIVE violated refresh recovery");
            bank.open = true;
            bank.active = now;
            bank.row = pin.address;
            break;
        }
        case SdramCommand::Read:
        case SdramCommand::Write: {
            const auto& bank = banks_[pin.bank];
            expect(bank.open && now >= bank.active + config.t_rcd, "column command violated tRCD");
            expect(remaining_ == 0, "column command overlapped previous bounded burst");
            expect((pin.address & 7u) == 0 && pin.address < 256,
                   "native fixed BL8 transactions start at an aligned legal column");
            writing_ = pin.command == SdramCommand::Write;
            remaining_ = 8;
            transferred_ = 0;
            data_bank_ = pin.bank;
            burst_word_ = (bank.row << 10) | (static_cast<unsigned>(pin.bank) << 8) | pin.address;
            data_at_ = now + (writing_ ? 0 : config.cas_latency);
            break;
        }
        }
        const bool expect_data = remaining_ != 0 && now == data_at_;
        expect(pin.data_valid == expect_data, "DQ valid edge disagrees with native BL8/CAS schedule");
        if (!expect_data) return;
        expect(pin.data_write == writing_, "DQ direction disagrees with column command");
        if (seen_data_ && prior_write_ != writing_) {
            const auto turn = prior_write_ ? config.write_to_read : config.read_to_write;
            expect(now >= prior_data_ + turn, "DQ bus changed direction before selected turnaround");
        }
        const auto address = (burst_word_ + transferred_) * 4;
        if (writing_) {
            auto word = value(address);
            for (unsigned byte = 0; byte < 4; ++byte) {
                if ((pin.dqm & (1u << byte)) != 0) continue;
                const auto mask = 0xffu << (8 * byte);
                word = (word & ~mask) | (pin.data & mask);
            }
            storage_[address / 4] = word;
            banks_[data_bank_].last_write = now;
        } else {
            expect(read_dqm == 0, "read DQM was not released ahead of the pipelined SDR data beats");
            expect(pin.data == value(address), "read DQ disagrees with observed physical write beats");
        }
        seen_data_ = true;
        prior_write_ = writing_;
        prior_data_ = now;
        ++transferred_;
        --remaining_;
        ++data_at_;
    }
};

void edge(SdramController& memory, PinObserver* observer = nullptr) {
    memory.tick();
    if (observer) observer->edge(memory);
}

void boot(SdramController& memory, PinObserver* observer = nullptr) {
    for (unsigned guard = 0; !memory.initialized(); ++guard) {
        expect(guard < 50000, "SDRAM initialization did not complete");
        edge(memory, observer);
    }
}

MemoryRequest write(std::uint32_t address, unsigned words, std::uint32_t seed = 0x10203040u) {
    MemoryRequest request;
    request.write = true;
    request.address = address;
    request.words = words;
    for (unsigned word = 0; word < words; ++word) {
        request.data[word] = seed ^ (0x08040201u * word);
        request.masks[word] = 0xf;
    }
    return request;
}

MemoryRequest read(std::uint32_t address, unsigned words) {
    MemoryRequest request;
    request.address = address;
    request.words = words;
    return request;
}

MemoryResponse transact(SdramController& memory, MemoryClient client, const MemoryRequest& request,
                        PinObserver* observer = nullptr) {
    expect(memory.submit(client, request), "unexpected busy request slot");
    for (unsigned guard = 0; !memory.response_ready(client); ++guard) {
        expect(guard < 100000, "SDRAM transaction failed to retire");
        edge(memory, observer);
    }
    return memory.take_response(client);
}

void initialization_tests() {
    test("initialization drives PRECHARGE, two refreshes and mode register after 100us", [] {
        SdramController memory;
        PinObserver pins;
        const auto queued = write(0, 1);
        expect(memory.submit(MemoryClient::Host, queued), "bounded requests may queue during startup");
        while (!memory.initialized()) {
            expect(!memory.response_ready(MemoryClient::Host), "startup request completed before initialized");
            edge(memory, &pins);
        }
        const std::array<SdramCommand, 4> expected{{SdramCommand::PrechargeAll,
            SdramCommand::AutoRefresh, SdramCommand::AutoRefresh, SdramCommand::LoadMode}};
        expect(pins.commands.size() == expected.size(), "startup issued a data command");
        for (std::size_t i = 0; i < expected.size(); ++i)
            expect(pins.commands[i].second == expected[i], "startup command order differs");
        expect(pins.commands.front().first == 10080, "100.8MHz startup wait must cover100us");
        expect(memory.statistics().initialization_cycles == memory.cycles(), "initialization clocks missing");
        while (!memory.response_ready(MemoryClient::Host)) edge(memory, &pins);
        memory.take_response(MemoryClient::Host);
        expect(memory.inspect_word(0) == queued.data[0], "queued startup write lost");
    });

    test("controller configuration rejects under-refresh and shortened power wait", [] {
        auto invalid = SdramConfig{};
        invalid.power_up_cycles = 10079;
        throws([&] { SdramController memory(invalid); }, "under100us startup accepted");
        invalid = {};
        invalid.refresh_interval_cycles = 1576;
        throws([&] { SdramController memory(invalid); }, "morethan15.625us refresh accepted");
        invalid = {};
        invalid.core_hz = 166000001;
        throws([&] { SdramController memory(invalid); }, "clock above documented166MHz accepted");
        invalid = {};
        invalid.cas_latency = 4;
        throws([&] { SdramController memory(invalid); }, "unsupported native CAS latency accepted");
        invalid = {};
        invalid.t_rc = 8;
        throws([&] { SdramController memory(invalid); }, "inconsistent row-cycle minimum accepted");
    });
}

void storage_tests() {
    test("unwritten SDRAM is observable poison and byte enables preserve other lanes", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        expect(memory.inspect_word(0) == 0xcdcdcdcdu, "SDRAM must not pretend a hardware zero INIT");
        auto request = write(0, 1, 0xa1b2c3d4u);
        request.masks[0] = 5;
        transact(memory, MemoryClient::Host, request, &pins);
        expect(memory.inspect_word(0) == 0xcdb2cdd4u, "DQM byte enables or unwritten bytes differ");
        const auto response = transact(memory, MemoryClient::Command, read(0, 1), &pins);
        expect(response.data[0] == 0xcdb2cdd4u && memory.statistics().uninitialized_read_words == 1,
               "useful uninitialized read must be reported");
        request = write(0, 1, 0x55667788u);
        request.masks[0] = 0xa;
        transact(memory, MemoryClient::Host, request, &pins);
        expect(memory.inspect_word(0) == 0x55b277d4u, "second masked write destroyed other lanes");
        expect(memory.inspect_word(4) == 0xcdcdcdcdu, "native write burst padding changed neighbor");
    });

    test("unaligned bounded transfers become three real aligned BL8 transactions", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        auto request = write(12, 16);
        request.tag = 0xabcdefu;
        auto response = transact(memory, MemoryClient::Host, request, &pins);
        expect(response.tag == request.tag && response.words == 16, "write completion lost request identity");
        expect(memory.statistics().write_commands == 3 && memory.statistics().physical_write_beats == 24,
               "fixed BL8 padding was omitted from bus occupancy");
        response = transact(memory, MemoryClient::Texture, read(12, 16), &pins);
        expect(memory.statistics().read_commands == 3 && memory.statistics().physical_read_beats == 24,
               "unaligned read did not use three native bursts");
        for (unsigned word = 0; word < 16; ++word)
            expect(response.data[word] == request.data[word], "unaligned reconstruction changed payload");
        expect(memory.inspect_word(8) == 0xcdcdcdcdu && memory.inspect_word(76) == 0xcdcdcdcdu,
               "masked burst prefix/suffix overwrote an out-of-request word");
    });

    test("column boundaries move across bank geometry without native burst wrap", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        const auto request = write(250 * 4, 16, 0x98175b03u);
        transact(memory, MemoryClient::Host, request, &pins);
        const auto response = transact(memory, MemoryClient::Tile, read(request.address, 16), &pins);
        for (unsigned word = 0; word < 16; ++word)
            expect(response.data[word] == request.data[word], "bank/column split changed linear address");
        expect(memory.statistics().activates == 2, "cross-bank contiguous request did not open two banks");
    });

    test("last SDRAM word is legal while overflow and overlarge requests reject before mutation", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        transact(memory, MemoryClient::Host, write(0x7ffffc, 1), &pins);
        expect(memory.inspect_word(0x7ffffc) == 0x10203040u, "last legal memory word lost");
        const auto before = memory.cycles();
        throws([&] { memory.submit(MemoryClient::Host, write(0x7ffffc, 2)); }, "extent overrun accepted");
        throws([&] { memory.submit(MemoryClient::Host, read(0xfffffffcu, 1)); }, "wrapped high address accepted");
        throws([&] { memory.submit(MemoryClient::Host, read(1, 1)); }, "unaligned address accepted");
        auto invalid = read(0, 1);
        invalid.words = 17;
        throws([&] { memory.submit(MemoryClient::Host, invalid); }, "unbounded request accepted");
        invalid.words = 0;
        throws([&] { memory.submit(MemoryClient::Host, invalid); }, "zero-length request accepted");
        throws([&] { memory.submit(MemoryClient::Count, read(0, 1)); }, "invalid client accepted");
        expect(memory.cycles() == before && memory.idle(), "failed request changed the timed stream");
    });
}

void streaming_slot_tests() {
    test("streamed slots use synchronous native ports and never consume metadata payload arrays", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        static_assert(SdramController::storage_bsram_blocks == 12);
        auto request = write(12, 16, 0x731925afu);
        request.tag = 0x20c0ffeeu;
        auto metadata = request;
        metadata.data.fill(0xdeadbeefu);
        metadata.masks.fill(0xffu); // Not device payload/masks for submit_prepared.
        expect(!memory.submit_prepared(MemoryClient::Host, metadata), "unfilled write slot was submitted");
        for (unsigned word = 0; word < request.words; ++word) {
            expect(memory.write_request_word(MemoryClient::Host, word, request.data[word], 0xf),
                   "stream input word rejected");
            expect(!memory.write_request_word(MemoryClient::Host, word, 0, 0),
                   "two input words consumed one physical port edge");
            expect(!memory.submit_prepared(MemoryClient::Host, metadata),
                   "write submitted before its pending RAM word committed");
            edge(memory, &pins);
        }
        expect(memory.submit_prepared(MemoryClient::Host, metadata), "completed write fill did not queue");
        expect(!memory.write_request_word(MemoryClient::Host, 0, 0, 0xf), "owned slot accepted another fill");
        while (!memory.response_ready(MemoryClient::Host)) edge(memory, &pins);
        const auto written = memory.response_info(MemoryClient::Host);
        expect(written.words == 16 && written.tag == request.tag, "stream write ACK metadata changed");
        throws([&] { memory.request_response_word(MemoryClient::Host, 0); },
               "write ACK offered nonexistent read data");
        memory.release_response(MemoryClient::Host);

        metadata = read(request.address, request.words);
        metadata.tag = 123;
        expect(memory.submit_prepared(MemoryClient::Host, metadata), "read metadata did not queue");
        while (!memory.response_ready(MemoryClient::Host)) edge(memory, &pins);
        expect(memory.response_info(MemoryClient::Host).tag == 123, "read identity changed");
        for (unsigned word = 0; word < request.words; ++word) {
            expect(memory.request_response_word(MemoryClient::Host, word), "response RAM read was rejected");
            expect(!memory.response_word_valid(MemoryClient::Host), "new read was valid without a RAM edge");
            throws([&] { memory.response_word(MemoryClient::Host); }, "new result was consumed on its request edge");
            expect(!memory.request_response_word(MemoryClient::Host, word), "two reads shared one physical edge");
            throws([&] { memory.release_response(MemoryClient::Host); }, "pending RAM read lost its ownership");
            edge(memory, &pins);
            expect(memory.response_word_valid(MemoryClient::Host) &&
                   memory.response_word_offset(MemoryClient::Host) == word &&
                   memory.response_word(MemoryClient::Host) == request.data[word],
                   "synchronous read did not return the selected slot word");
        }
        expect(!memory.submit_prepared(MemoryClient::Host, read(0, 1)), "held stream response did not backpressure");
        throws([&] { memory.request_response_word(MemoryClient::Host, 16); }, "response index overflow was accepted");
        memory.release_response(MemoryClient::Host);
        expect(!memory.response_word_valid(MemoryClient::Host), "released response retained output validity");
        const auto& stats = memory.statistics();
        expect(stats.prepared_submissions == 2 && stats.legacy_submissions == 0 && stats.legacy_response_reads == 0,
               "streamed requests entered the host-only bulk adapter");
        expect(stats.slot_write_words == 32 && stats.slot_read_words == 32,
               "slot port traffic omitted input/refill writes or engine/client reads");
        expect(stats.write_commands == 3 && stats.read_commands == 3,
               "slot buffering changed required native unaligned BL8 transactions");
    });

    test("stream fills reject stale words and support explicit partial-fill recovery", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        auto metadata = write(0, 2);
        throws([&] { memory.write_request_word(MemoryClient::Copy, 16, 0, 0xf); }, "slot word overflow accepted");
        throws([&] { memory.write_request_word(MemoryClient::Copy, 0, 0, 0x10); }, "slot byte-mask overflow accepted");
        expect(memory.write_request_word(MemoryClient::Copy, 0, 0x12345678u, 5), "first fill rejected");
        memory.discard_request_words(MemoryClient::Copy); // Cancel before its capture edge.
        edge(memory, &pins);
        expect(memory.statistics().slot_write_words == 0, "cancelled input still wrote RAM");
        expect(!memory.submit_prepared(MemoryClient::Copy, metadata), "discard left stale fill validity");
        for (unsigned word = 0; word < 2; ++word) {
            expect(memory.write_request_word(MemoryClient::Copy, word, 0xa1b2c3d4u + word, 5), "fill word rejected");
            edge(memory, &pins);
        }
        expect(memory.submit_prepared(MemoryClient::Copy, metadata), "complete refill rejected");
        throws([&] { memory.discard_request_words(MemoryClient::Copy); }, "recovery discarded an owned request");
        while (!memory.response_ready(MemoryClient::Copy)) edge(memory, &pins);
        memory.release_response(MemoryClient::Copy);
        expect(memory.inspect_word(0) == 0xcdb2cdd4u && memory.inspect_word(4) == 0xcdb2cdd5u,
               "streamed write masks did not preserve undefined lanes");
        expect(memory.write_request_word(MemoryClient::Copy, 0, 7, 0xf), "second fill rejected");
        edge(memory, &pins);
        expect(!memory.submit_prepared(MemoryClient::Copy, metadata), "prior request's second word was reused");
        memory.discard_request_words(MemoryClient::Copy);
        expect(memory.submit_prepared(MemoryClient::Copy, read(0, 1)), "read blocked by discarded partial fill");
        while (!memory.response_ready(MemoryClient::Copy)) edge(memory, &pins);
        expect(memory.request_response_word(MemoryClient::Copy, 0), "response read rejected");
        edge(memory, &pins);
        expect(memory.response_word(MemoryClient::Copy) == 0xcdb2cdd4u, "partial fill changed SDRAM without submission");
        memory.release_response(MemoryClient::Copy);
    });

    test("six physical client slot pairs retain independent synchronous outputs", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        for (unsigned index = 0; index < memory_client_count; ++index) {
            const auto client = static_cast<MemoryClient>(index);
            expect(memory.write_request_word(client, 0, 0x12890000u + index, 0xf), "independent input port rejected");
        }
        edge(memory, &pins);
        for (unsigned index = 0; index < memory_client_count; ++index) {
            auto metadata = write(index * 32, 1);
            expect(memory.submit_prepared(static_cast<MemoryClient>(index), metadata), "independent write submit rejected");
        }
        for (unsigned index = 0; index < memory_client_count; ++index) {
            const auto client = static_cast<MemoryClient>(index);
            while (!memory.response_ready(client)) edge(memory, &pins);
            memory.release_response(client);
            expect(memory.submit_prepared(client, read(index * 32, 1)), "independent read submit rejected");
        }
        bool complete = false;
        while (!complete) {
            complete = true;
            for (unsigned index = 0; index < memory_client_count; ++index)
                complete = complete && memory.response_ready(static_cast<MemoryClient>(index));
            if (!complete) edge(memory, &pins);
        }
        for (unsigned index = 0; index < memory_client_count; ++index)
            expect(memory.request_response_word(static_cast<MemoryClient>(index), 0), "independent read port rejected");
        edge(memory, &pins);
        for (unsigned index = 0; index < memory_client_count; ++index) {
            const auto client = static_cast<MemoryClient>(index);
            expect(memory.response_word(client) == 0x12890000u + index, "client RAM output aliasing");
            memory.release_response(client);
        }
    });
}

void timing_tests() {
    test("open-row hits avoid ACT while same-bank row conflicts pay PRE and ACT", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        transact(memory, MemoryClient::Host, write(0, 8), &pins);
        const auto active = memory.statistics().activates;
        const auto precharge = memory.statistics().precharges;
        transact(memory, MemoryClient::Host, write(32, 8), &pins);
        expect(memory.statistics().activates == active && memory.statistics().precharges == precharge,
               "same-row traffic should retain an open bank");
        const auto hit_service = memory.statistics().clients[1].max_service_cycles;
        transact(memory, MemoryClient::Host, write(4096, 8), &pins);
        expect(memory.statistics().activates == active + 1 && memory.statistics().precharges == precharge + 1,
               "row conflict did not drive native precharge/activate commands");
        expect(memory.statistics().clients[1].max_service_cycles > hit_service,
               "conflicting row should incur measured additional service clocks");
    });

    test("native CL2 and CL3 produce distinct read-data and completion latency", [] {
        std::array<std::uint64_t, 2> elapsed{};
        for (unsigned choice = 0; choice < 2; ++choice) {
            SdramConfig config;
            config.cas_latency = choice + 2;
            SdramController memory(config);
            PinObserver pins;
            boot(memory, &pins);
            transact(memory, MemoryClient::Host, write(0, 8), &pins);
            const auto before = memory.cycles();
            transact(memory, MemoryClient::Host, read(0, 8), &pins);
            elapsed[choice] = memory.cycles() - before;
        }
        expect(elapsed[1] == elapsed[0] + 1, "CAS latency is only a label instead of timed behavior");
    });

    test("selected timing changes alter physical traffic without losing data", [] {
        SdramConfig config;
        config.t_rp = 4;
        config.t_rcd = 5;
        config.t_rfc = 13;
        config.t_mrd = 4;
        config.t_wr = 6;
        config.t_ras = 14;
        config.t_rc = 23; // Stronger than tRAS+tRP: refresh must also wait tRC.
        config.read_to_write = 7;
        config.write_to_read = 6;
        config.refresh_interval_cycles = 700;
        SdramController memory(config);
        PinObserver pins;
        boot(memory, &pins);
        for (unsigned turn = 0; turn < 80; ++turn) {
            const auto address = ((turn & 1u) * 4096) + 4 * (turn % 7);
            const auto request = write(address, 16, turn);
            transact(memory, MemoryClient::Tile, request, &pins);
            const auto result = transact(memory, MemoryClient::Texture, read(address, 16), &pins);
            for (unsigned word = 0; word < 16; ++word)
                expect(result.data[word] == request.data[word], "stretched physical timings changed data");
        }
        expect(memory.statistics().refreshes > 2 && memory.statistics().refresh_late_cycles == 0 &&
               memory.statistics().max_refresh_gap_cycles <= config.refresh_interval_cycles,
               "refresh deadlines failed under timing/row conflict stress");
    });
}

void arbitration_tests() {
    test("client slots apply backpressure through response consumption", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        auto request = write(0, 16);
        request.tag = 7;
        expect(memory.submit(MemoryClient::Host, request), "first request should own slot");
        expect(!memory.submit(MemoryClient::Host, write(64, 1)), "occupied request slot accepted replacement");
        throws([&] { memory.take_response(MemoryClient::Host); }, "unestablished response consumed");
        while (!memory.response_ready(MemoryClient::Host)) edge(memory, &pins);
        for (unsigned cycle = 0; cycle < 100; ++cycle) edge(memory, &pins);
        expect(!memory.submit(MemoryClient::Host, write(64, 1)), "unconsumed response did not hold slot");
        const auto readback = transact(memory, MemoryClient::Command, read(0, 16), &pins);
        expect(readback.data[15] == request.data[15], "one client's held response blocked other clients");
        const auto original = memory.take_response(MemoryClient::Host);
        expect(original.tag == 7, "held response identity overwritten");
        expect(memory.submit(MemoryClient::Host, write(64, 1)), "taking response did not release slot");
    });

    test("bounded scanout priority cannot starve any of the five competing clients", [] {
        SdramConfig config;
        config.scanout_priority_limit = 2;
        SdramController memory(config);
        PinObserver pins;
        boot(memory, &pins);
        for (unsigned index = 0; index < memory_client_count; ++index)
            expect(memory.submit(static_cast<MemoryClient>(index), write(index * 64, 8, index)),
                   "one outstanding request per client should be accepted");
        std::array<bool, memory_client_count> completed{};
        unsigned streak = 0;
        unsigned other_completions = 0;
        for (unsigned cycle = 0; other_completions != memory_client_count - 1; ++cycle) {
            expect(cycle < 3000, "a client was starved by sustained display priority");
            edge(memory, &pins);
            for (unsigned index = 0; index < memory_client_count; ++index) {
                const auto client = static_cast<MemoryClient>(index);
                if (!memory.response_ready(client)) continue;
                memory.take_response(client);
                if (index == 0) {
                    ++streak;
                    expect(streak <= config.scanout_priority_limit, "scanout exceeded bounded priority quota");
                    expect(memory.submit(client, write(0, 8)), "display refill did not requeue");
                } else {
                    expect(!completed[index], "non-display client completed twice");
                    completed[index] = true;
                    ++other_completions;
                    streak = 0;
                }
            }
        }
        for (unsigned index = 1; index < memory_client_count; ++index)
            expect(completed[index] && memory.statistics().clients[index].max_queue_wait_cycles < 3000,
                   "arbiter failed bounded progress");
    });

    test("refresh continues while completed response is held and retains write contents", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        expect(memory.submit(MemoryClient::Host, write(0, 16)), "write rejected");
        while (!memory.response_ready(MemoryClient::Host)) edge(memory, &pins);
        const auto before = memory.statistics().refreshes;
        for (unsigned cycle = 0; cycle < 3 * memory.config().refresh_interval_cycles; ++cycle)
            edge(memory, &pins);
        expect(memory.statistics().refreshes >= before + 3, "held response blocked periodic refresh");
        expect(memory.response_ready(MemoryClient::Host), "refresh discarded completion");
        memory.take_response(MemoryClient::Host);
        const auto result = transact(memory, MemoryClient::Copy, read(0, 16), &pins);
        expect(result.data[15] == write(0, 16).data[15], "refresh changed stored data");
        expect(memory.statistics().refresh_late_cycles == 0 &&
               memory.statistics().max_refresh_gap_cycles <= memory.config().refresh_interval_cycles,
               "retention interval exceeded under held-client stress");
    });
}

void randomized_test() {
    test("random masked traffic matches independent pin-level memory and timing observer", [] {
        SdramController memory;
        PinObserver pins;
        boot(memory, &pins);
        std::mt19937 random(0x20ca5a11u);
        for (unsigned visit = 0; visit < 600; ++visit) {
            const auto address = static_cast<std::uint32_t>((random() % 16300) * 4);
            const auto words = 1u + static_cast<unsigned>(random() % 16);
            auto request = write(address, words, static_cast<std::uint32_t>(random()));
            for (unsigned word = 0; word < words; ++word)
                request.masks[word] = static_cast<std::uint8_t>(random() & 15u);
            const auto client = static_cast<MemoryClient>(random() % memory_client_count);
            transact(memory, client, request, &pins);
            const auto response = transact(memory, client, read(address, words), &pins);
            for (unsigned word = 0; word < words; ++word) {
                expect(response.data[word] == pins.value(address + word * 4),
                       "completed response disagrees with reconstructed physical device");
                expect(memory.inspect_word(address + word * 4) == pins.value(address + word * 4),
                       "diagnostic backing store disagrees with DQ traffic");
            }
        }
        expect(memory.statistics().refreshes > 2 && memory.statistics().refresh_late_cycles == 0 &&
               memory.statistics().max_refresh_gap_cycles <= memory.config().refresh_interval_cycles,
               "random traffic deferred mandatory refresh");
    });
}

} // namespace

int main() {
    initialization_tests();
    storage_tests();
    streaming_slot_tests();
    timing_tests();
    arbitration_tests();
    randomized_test();
    std::cout << cases - failures << '/' << cases << " SDRAM controller/device cases passed\n";
    return failures == 0 ? 0 : 1;
}
