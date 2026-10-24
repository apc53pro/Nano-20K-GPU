#include "nano_gpu/demo.hpp"
#include "nano_gpu/hw/protocol.hpp"
#include "nano_gpu/hw/resources.hpp"
#include "nano_gpu/hw/system.hpp"
#include "nano_gpu/hw/throughput.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace nano_gpu;
using namespace nano_gpu::hw;

unsigned cases = 0, failures = 0;
constexpr std::uint64_t test_budget = 150000000;
constexpr std::uint64_t frame_cycles = 800u * 525u * 4u;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Operation> void expect_throws(Operation&& operation, const std::string& message) {
    bool threw = false;
    try { operation(); } catch (const std::exception&) { threw = true; }
    expect(threw, message);
}
template<class Operation> void run_case(const char* name, Operation&& operation) {
    ++cases;
    try { operation(); }
    catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}
template<class Condition> void wait_until(SystemModel& model, Condition&& condition,
                                         std::uint64_t budget = test_budget) {
    for (std::uint64_t elapsed = 0; !condition(); ++elapsed) {
        if (elapsed == budget) throw std::runtime_error("system test exhausted its bounded progress budget");
        if (model.faulted()) throw std::runtime_error("unexpected system fault: " + model.fault_message());
        model.tick();
    }
}
void boot(SystemModel& model) { wait_until(model, [&] { return model.ready(); }, 10000000); }
std::vector<std::uint8_t> scalar(std::uint32_t value) {
    std::vector<std::uint8_t> bytes(4); write_u32(bytes, 0, value); return bytes;
}
void append(std::vector<std::uint8_t>& target, const std::vector<std::uint8_t>& tail) {
    target.insert(target.end(), tail.begin(), tail.end());
}

// External CPU test driver: no privileged access to device queues or memory.
// Controls are established after the previous transfer edge and remain stable
// through the selected setup/hold interval, including receiver backpressure.
class RawHost {
public:
    explicit RawHost(SystemModel& model) : device(model) {}
    void align() {
        const auto& config = device.config().host;
        const auto phase = (config.transfer_phase + 1) % config.clock_divider;
        while (device.host_statistics().cycles % config.clock_divider != phase) device.tick();
    }
    void send(const std::vector<std::uint8_t>& bytes) {
        align();
        std::size_t cursor = 0;
        for (std::uint64_t elapsed = 0; cursor < bytes.size(); ++elapsed) {
            expect(elapsed < test_budget, "raw host was indefinitely backpressured");
            HostPins pins; pins.write_valid = true; pins.data = bytes[cursor];
            if (device.tick(pins).write_accepted) ++cursor;
            if (device.faulted() && cursor != bytes.size())
                throw std::runtime_error("raw host fault before all request bytes were accepted: " + device.fault_message());
        }
        device.advance(device.config().host.hold_cycles);
    }
    std::uint8_t status_byte(unsigned address) {
        expect(address > 0 && address < 64, "invalid test status register");
        align();
        HostPins pins; pins.read = true; pins.register_address = static_cast<std::uint8_t>(address);
        for (unsigned edge = 0; edge < 3 * device.config().host.clock_divider + 4; ++edge) {
            const auto result = device.tick(pins);
            if (result.read_valid) { device.tick(); return result.read_data; }
        }
        throw std::runtime_error("sideband status read did not complete");
    }
    std::array<std::uint8_t, 64> status() {
        std::array<std::uint8_t, 64> snapshot{};
        snapshot[1] = status_byte(1); // Atomically captures all status fields.
        for (unsigned byte = 2; byte < snapshot.size(); ++byte) snapshot[byte] = status_byte(byte);
        return snapshot;
    }
    std::vector<std::uint8_t> reply(Opcode opcode, std::uint32_t sequence) {
        align();
        std::vector<std::uint8_t> bytes;
        std::size_t length = packet_header_bytes;
        for (std::uint64_t elapsed = 0; bytes.size() < length; ++elapsed) {
            expect(elapsed < test_budget, "raw reply did not complete");
            expect(!device.faulted(), "raw reply blocked by device fault: " + device.fault_message());
            HostPins pins; pins.read = true;
            const auto result = device.tick(pins);
            if (!result.read_valid) continue;
            bytes.push_back(result.read_data);
            if (bytes.size() == packet_header_bytes) {
                expect(read_u32(bytes, 0) == 0x5550474eu && bytes[4] == 0 && bytes[5] == 1 &&
                       read_u16(bytes, 6) == (static_cast<std::uint16_t>(opcode) | 0x8000u) &&
                       read_u32(bytes, 12) == sequence && read_u32(bytes, 8) <= 780,
                       "raw reply envelope or command identity differs");
                length += read_u32(bytes, 8);
            }
        }
        device.tick();
        expect(bytes.size() >= 28 && read_u32(bytes, 16) == device.system_statistics().epoch &&
               read_u32(bytes, 20) == 0, "raw reply has stale epoch or failed completion");
        return bytes;
    }
    std::vector<std::uint8_t> read_memory(std::uint32_t sequence, std::uint32_t address,
                                         std::uint32_t length, std::uint32_t token) {
        auto payload = scalar(address); append(payload, scalar(length)); append(payload, scalar(token));
        send(encode_packet(Opcode::ReadMemory, sequence, payload));
        const auto result = reply(Opcode::ReadMemory, sequence);
        expect(result.size() == 36 + length && read_u32(result, 24) == token &&
               read_u32(result, 28) == address && read_u32(result, 32) == length,
               "READ_MEMORY response lost address, length, or token");
        return {result.begin() + 36, result.end()};
    }
private:
    SystemModel& device;
};

std::uint32_t status_u32(const std::array<std::uint8_t, 64>& bytes, unsigned offset) {
    std::uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; ++byte) value |= std::uint32_t{bytes[offset + byte]} << (8 * byte);
    return value;
}
void compare_outputs(const Renderer& expected, const SystemModel& actual) {
    expect(actual.color(0) == expected.color(0), "physical color buffer0 differs from canonical SRR");
    expect(actual.color(1) == expected.color(1), "physical color buffer1 differs from canonical SRR");
    expect(actual.depth() == expected.depth(), "shared D24 buffer differs from canonical SRR");
    expect(actual.front_id() == expected.front_id(), "front-buffer ownership differs");
}
void compare_counts(const Statistics& expected, const Statistics& actual) {
    expect(actual.tile_references == expected.tile_references && actual.tiles_processed == expected.tiles_processed &&
           actual.candidates == expected.candidates && actual.covered_fragments == expected.covered_fragments &&
           actual.depth_rejected == expected.depth_rejected && actual.keyed_rejected == expected.keyed_rejected &&
           actual.texture_samples == expected.texture_samples && actual.color_writes == expected.color_writes &&
           actual.depth_writes == expected.depth_writes, "logical draw counters differ from canonical SRR");
}
template<class Word> std::uint64_t buffer_hash(const std::vector<Word>& words) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto word : words) for (unsigned byte = 0; byte < sizeof(Word); ++byte) {
        hash ^= static_cast<std::uint8_t>(word >> (8 * byte)); hash *= 1099511628211ull;
    }
    return hash;
}
std::shared_ptr<Image> pattern(int width = 8, int height = 8, int pitch = 20) {
    auto image = std::make_shared<Image>(width, height, pitch);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        image->at(x, y) = ((x + 2 * y) % 7 == 0) ? 0 :
            static_cast<std::uint16_t>(0x0421u + 0x181u * x + 0x423u * y);
    return image;
}
Triangle small_triangle(int offset, const RenderState& state) {
    const std::array<ScreenVertex, 3> vertices{{
        {static_cast<long double>(8 + offset), 8, 0.25L, 1, -1.25L, 0.5L, 240, 20, 80},
        {static_cast<long double>(24 + offset), 10, 0.65L, 2, 5.75L, -2, 20, 240, 80},
        {static_cast<long double>(10 + offset), 27, 0.45L, 4, -1, 6.75L, 80, 20, 240}}};
    const auto triangle = setup_triangle(vertices, state);
    expect(triangle.has_value(), "test geometry was unexpectedly culled");
    return *triangle;
}
Pass mixed_pass(int target = 1) {
    auto image = pattern();
    Pass pass; pass.target = target; pass.clear_color = 0x1234; pass.clear_depth = 0xfedcba;
    for (unsigned shade = 0; shade < 5; ++shade) {
        RenderState state; state.shade = static_cast<ShadeMode>(shade);
        state.color = {180, 90, 220}; state.depth_func = DepthFunc::LessEqual;
        state.address_u = AddressMode::Repeat; state.address_v = AddressMode::Clamp;
        if (shade >= 2) { state.texture = image; state.color_key = 0; }
        pass.primitives.emplace_back(small_triangle(25 * static_cast<int>(shade), state));
    }
    pass.primitives.emplace_back(Rect{{-4, 33, 38, 44}, overlay_state({10, 20, 30}), 123});
    pass.primitives.emplace_back(Rect{{44, 35, 45, 36}, overlay_state({240, 40, 80}), 0});
    pass.primitives.emplace_back(Line{{639, 47}, {0, 45}, overlay_state({70, 180, 240}), 0});
    auto sprite_state = overlay_state(); sprite_state.texture = pattern(16, 16, 36); sprite_state.color_key = 0;
    sprite_state.scissor = {0, 51, 5, 56};
    pass.primitives.emplace_back(Sprite{{-3, 49, 5, 57}, 3, 4, sprite_state, 0});
    return pass;
}
Pass clear_pass(int target, std::uint16_t color = 0) {
    Pass pass; pass.target = target; pass.clear_color = color; return pass;
}

void boot_and_io_tests() {
    run_case("timed boot initializes both physical colors and every D24 word before ready", [] {
        SystemModel model;
        expect(!model.ready() && !model.color_valid(0) && !model.color_valid(1) && !model.depth_valid(),
               "power-on state advertised initialized attachments");
        model.advance(1000);
        expect(!model.ready() && model.memory_statistics().clients[static_cast<unsigned>(MemoryClient::Tile)].bytes_written == 0,
               "attachments became available before the SDRAM power-up wait");
        boot(model);
        expect(model.color_valid(0) && model.color_valid(1) && model.depth_valid() && model.front_id() == 0,
               "boot did not establish initial ownership and contents");
        expect(model.system_statistics().boot_cycles >= model.config().memory.core_hz / 10000 &&
               model.memory_statistics().clients[static_cast<unsigned>(MemoryClient::Tile)].bytes_written == heap_base,
               "boot bypassed physical writes or initialization timing");
        RawHost host(model);
        unsigned sequence = 1;
        for (const auto address : std::array<std::uint32_t, 4>{0, color_buffer_bytes - 4,
                 color_buffer_base(1), depth_buffer_base - 4}) {
            const auto bytes = host.read_memory(sequence++, address, 4, address);
            expect(read_u32(bytes, 0) == 0, "physical boot color word was not black");
        }
        for (const auto address : std::array<std::uint32_t, 2>{depth_buffer_base, heap_base - 4}) {
            const auto bytes = host.read_memory(sequence++, address, 4, address);
            expect(read_u32(bytes, 0) == depth_max, "physical boot depth word was not far D24");
        }
        expect(model.memory_statistics().uninitialized_read_words == 0 &&
               model.scanout_statistics().startup_black_pixels != 0, "startup exposed undefined memory");
    });
    run_case("ordered byte-masked heap uploads are observable only through timed readback", [] {
        SystemModel model; boot(model); RawHost host(model);
        std::vector<std::uint8_t> program;
        auto payload = scalar(heap_base); append(payload, scalar(8));
        payload.insert(payload.end(), {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17});
        append(program, encode_packet(Opcode::WriteHeap, 1, payload));
        payload = scalar(heap_base + 1); append(payload, scalar(5));
        payload.insert(payload.end(), {0xa1, 0xb2, 0xc3, 0xd4, 0xe5});
        append(program, encode_packet(Opcode::WriteHeap, 2, payload));
        append(program, encode_packet(Opcode::Fence, 3, scalar(0x12345678)));
        host.send(program);
        const auto fence = host.reply(Opcode::Fence, 3);
        expect(read_u32(fence, 24) == 0x12345678, "fence token changed");
        const auto bytes = host.read_memory(4, heap_base, 8, 0x87654321);
        expect(bytes == std::vector<std::uint8_t>({0x10, 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0x16, 0x17}),
               "unaligned upload lost masked neighbors or violated ordering");
        const auto& memory = model.memory_statistics();
        expect(memory.clients[static_cast<unsigned>(MemoryClient::Host)].bytes_written == 13 &&
               memory.clients[static_cast<unsigned>(MemoryClient::Command)].bytes_read >= 8 &&
               model.host_statistics().tx_bytes == fence.size() + 44,
               "readback or upload bypassed the timed device ports");
    });
    run_case("unaligned maximum readback preserves the reader guard word through clocked ports", [] {
        SystemModel model; boot(model); RawHost host(model);
        std::vector<std::uint8_t> expected(520);
        for (unsigned byte = 0; byte < expected.size(); ++byte)
            expected[byte] = static_cast<std::uint8_t>(byte * 37u + (byte >> 8) * 19u);
        auto payload = scalar(heap_base); append(payload, scalar(static_cast<std::uint32_t>(expected.size())));
        append(payload, expected);
        auto program = encode_packet(Opcode::WriteHeap, 1, payload);
        append(program, encode_packet(Opcode::Fence, 2, scalar(0x11223344)));
        host.send(program); (void)host.reply(Opcode::Fence, 2);
        const auto before = model.memory_statistics().clients[static_cast<unsigned>(MemoryClient::Command)].bytes_read;
        const auto actual = host.read_memory(3, heap_base + 1, 512, 0x55667788);
        expect(actual == std::vector<std::uint8_t>(expected.begin() + 1, expected.begin() + 513),
               "unaligned 512-byte response lost its final aligned guard word");
        const auto after = model.memory_statistics().clients[static_cast<unsigned>(MemoryClient::Command)].bytes_read;
        expect(after - before == 516, "unaligned readback did not fetch its complete aligned extent");
        expect(host.read_memory(4, heap_base + 512, 8, 0x99aabbcc) ==
               std::vector<std::uint8_t>(expected.begin() + 512, expected.end()),
               "readback staging changed adjacent uploaded bytes");
        const auto& memory = model.memory_statistics();
        expect(memory.prepared_submissions != 0 && memory.slot_read_words != 0 && memory.slot_write_words != 0 &&
               memory.legacy_submissions == 0 && memory.legacy_response_reads == 0,
               "readback or upload used untimed legacy burst arrays");
    });
}

void rendering_tests() {
    run_case("all shading modes and mixed2D survive physical upload fetch cache and stores", [] {
        const auto pass = mixed_pass();
        const auto program = compile_pass(pass);
        Renderer expected; expected.render(pass);
        SystemModel actual; actual.render(pass); // Intentionally submits while boot is still busy.
        compare_outputs(expected, actual); compare_counts(expected.statistics(), actual.statistics());
        const auto& system = actual.system_statistics();
        const auto& host = actual.host_statistics();
        const auto& memory = actual.memory_statistics();
        expect(system.passes_completed == 1 && system.uploaded_bytes == program.upload_bytes &&
               system.fetched_record_bytes == program.reference_count * primitive_record_bytes &&
               system.fetched_bin_bytes == tile_count * tile_entry_bytes + 4 * program.reference_count,
               "records or ordered references arrived without physical fetch accounting");
        expect(host.rx_bytes == program.bytes.size() && host.cycles >= host.rx_bytes * actual.config().host.clock_divider &&
               host.backpressure_edges != 0 && host.rx_high_water <= actual.config().host.rx_fifo_capacity &&
               host.setup_violations == 0 && host.hold_violations == 0,
               "eight-bit delivery bypassed bandwidth, FIFO limits, or valid pin timing");
        expect(memory.clients[static_cast<unsigned>(MemoryClient::Texture)].bytes_read != 0 &&
               memory.clients[static_cast<unsigned>(MemoryClient::Command)].bytes_read != 0 &&
               memory.clients[static_cast<unsigned>(MemoryClient::Tile)].bytes_written > heap_base &&
               system.last_pass_cycles > actual.statistics().hardware_cycles,
               "system model omitted memory contention or external transfers");
        expect(memory.refreshes != 0 && memory.refresh_late_cycles == 0 && memory.uninitialized_read_words == 0,
               "normal workload deferred refresh or read undefined requested words");
    });
    run_case("original showcase matches golden RGB565 D24 and actual independent scanout", [] {
        const auto pass = make_demo_pass(DemoScene::Showcase, 1, 0);
        Renderer expected; expected.render(pass);
        SystemModel actual; actual.render(pass);
        compare_outputs(expected, actual); compare_counts(expected.statistics(), actual.statistics());
        expect(buffer_hash(actual.color(1)) == 0x1d85d76382a08afbULL &&
               buffer_hash(actual.depth()) == 0xcdb41a22de37f530ULL,
               "original showcase no longer matches its canonical time-zero hashes");
        expect(actual.statistics().texture_samples > 100000 && actual.statistics().keyed_rejected != 0 &&
               actual.memory_statistics().clients[static_cast<unsigned>(MemoryClient::Scanout)].bytes_read != 0,
               "showcase or concurrent scanout was replaced by a simpler workload");
        expect(actual.memory_statistics().legacy_submissions == 0 &&
               actual.memory_statistics().legacy_response_reads == 0 &&
               actual.memory_statistics().prepared_submissions != 0 &&
               actual.memory_statistics().slot_write_words != 0 && actual.memory_statistics().slot_read_words != 0,
               "showcase GPU execution bypassed the native client-slot memory ports");
        actual.queue_present(1); expected.queue_present(1);
        actual.vertical_blank(); expected.vertical_blank();
        compare_outputs(expected, actual);
        expect(actual.display_color() == expected.front_color() && actual.scanout_statistics().underflows == 0 &&
               actual.system_statistics().presentations_completed == 1 &&
               actual.system_statistics().last_present_wait_cycles != 0 &&
               actual.system_statistics().last_scanout_observation_cycles != 0,
               "SDL observation bypassed FIFO output, vblank presentation, or complete-frame scanout");
    });
    run_case("multipass LOAD presentation and second physical back preserve shared depth", [] {
        Renderer expected; SystemModel actual;
        auto pass = mixed_pass(); expected.render(pass); actual.render(pass);
        Pass overlay; overlay.target = 1; overlay.color_load = overlay.depth_load = LoadOp::Load;
        overlay.primitives.emplace_back(Rect{{10, 10, 60, 15}, overlay_state({240, 200, 50}), 0});
        expected.render(overlay); actual.render(overlay);
        compare_outputs(expected, actual); compare_counts(expected.statistics(), actual.statistics());
        expected.queue_present(1); actual.queue_present(1); expected.vertical_blank(); actual.vertical_blank();
        const auto displayed = actual.front_color();
        Pass next; next.target = 0; next.color_load = next.depth_load = LoadOp::Load;
        next.primitives.emplace_back(Rect{{30, 30, 42, 43}, overlay_state({50, 180, 210}), 0});
        expected.render(next); actual.render(next);
        compare_outputs(expected, actual);
        expect(actual.front_color() == displayed && actual.front_id() == 1, "rendering changed the displayed front");
        expected.queue_present(0); actual.queue_present(0); expected.vertical_blank(); actual.vertical_blank();
        compare_outputs(expected, actual);
        expect(actual.display_color() == expected.front_color() && actual.scanout_statistics().underflows == 0,
               "second physical buffer swap mixed old and new scanout data");
    });
    run_case("injected SDRAM latency changes elapsed cycles while preserving exact pixels", [] {
        const auto pass = mixed_pass();
        SystemConfig slow;
        slow.memory.t_rp = 10; slow.memory.t_rcd = 10; slow.memory.t_rfc = 25;
        slow.memory.t_ras = 20; slow.memory.t_rc = 30;
        SystemModel normal; SystemModel delayed(slow);
        boot(normal); boot(delayed);
        normal.render(pass); delayed.render(pass);
        expect(normal.color(0) == delayed.color(0) && normal.color(1) == delayed.color(1) &&
               normal.depth() == delayed.depth(), "memory latency changed rendering results");
        expect(delayed.system_statistics().last_pass_cycles > normal.system_statistics().last_pass_cycles &&
               delayed.system_statistics().last_submission_cycles > normal.system_statistics().last_submission_cycles &&
               delayed.memory_statistics().clients[static_cast<unsigned>(MemoryClient::Tile)].service_cycles >
                   normal.memory_statistics().clients[static_cast<unsigned>(MemoryClient::Tile)].service_cycles,
               "memory timing knobs did not stall the real execution path");
        compare_counts(normal.statistics(), delayed.statistics());
    });
    run_case("finite receive and packet FIFOs backpressure a faster external producer", [] {
        SystemConfig config;
        config.host.clock_divider = 1; config.host.transfer_phase = 0;
        config.host.setup_cycles = config.host.hold_cycles = 0;
        config.host.rx_fifo_capacity = 16; config.host.command_fifo_capacity = 1;
        config.host.response_fifo_capacity = 128;
        SystemModel actual(config); boot(actual);
        Pass pass;
        for (int item = 0; item < 20; ++item)
            pass.primitives.emplace_back(Rect{{item * 3, 5, item * 3 + 2, 8},
                overlay_state({static_cast<std::uint8_t>(item * 10), 90, 180}), 0});
        const auto program = compile_pass(pass);
        Renderer expected; expected.render(pass); actual.render(pass);
        compare_outputs(expected, actual);
        const auto& host = actual.host_statistics();
        expect(host.rx_bytes == program.bytes.size() && host.backpressure_edges != 0 &&
               host.rx_high_water == config.host.rx_fifo_capacity && host.command_high_water == 1 &&
               host.protocol_faults == 0 && host.unstable_writes == 0,
               "bounded external command producer lost bytes or escaped backpressure");
    });
}

void copy_tests() {
    run_case("NPOT image and overlapping framebuffer copies traverse shared SDRAM", [] {
        auto image = pattern(23, 17, 52);
        Renderer expected; SystemModel actual;
        expected.copy_image(*image, 1, {0, 0}, {20, 20}, 23, 17);
        actual.copy_image(*image, 1, {0, 0}, {20, 20}, 23, 17);
        expected.copy_image(*image, 1, {-2, -3}, {-4, 475}, 30, 25);
        actual.copy_image(*image, 1, {-2, -3}, {-4, 475}, 30, 25);
        for (const auto delta : std::array<Point, 4>{{{3, 2}, {-2, -3}, {0, 2}, {2, 0}}}) {
            expected.copy_rect(1, 1, {20, 20}, {20 + delta.x, 20 + delta.y}, 20, 14);
            actual.copy_rect(1, 1, {20, 20}, {20 + delta.x, 20 + delta.y}, 20, 14);
            compare_outputs(expected, actual);
            expect(actual.statistics().hardware_copy_words == 20u * 14u,
                   "clipped row copy omitted or duplicated words");
        }
        expected.queue_present(1); actual.queue_present(1); expected.vertical_blank(); actual.vertical_blank();
        expected.copy_rect(1, 0, {20, 20}, {100, 100}, 23, 17);
        actual.copy_rect(1, 0, {20, 20}, {100, 100}, 23, 17);
        compare_outputs(expected, actual);
        const auto& memory = actual.memory_statistics();
        expect(memory.clients[static_cast<unsigned>(MemoryClient::Copy)].bytes_read != 0 &&
               memory.clients[static_cast<unsigned>(MemoryClient::Copy)].bytes_written != 0 &&
               actual.system_statistics().copies_completed == 7 &&
               actual.display_color() == actual.front_color(), "copies or display bypassed modeled storage");
    });
}

void clock_observer_tests() {
    run_case("common-clock observer covers boot DMA drawing and presentation without replaying fragments", [] {
        SystemModel model;
        ThroughputMonitor monitor;
        std::uint64_t events = 0;
        model.set_cycle_observer([&](std::uint64_t clock, const FragmentCycle& event) {
            expect(clock == ++events, "common-clock observer skipped or duplicated an edge");
            monitor.observe(event, clock);
        });
        Pass pass;
        RenderState state; state.color = {30, 120, 210};
        pass.primitives.emplace_back(Rect{{0, 0, 32, 32}, state, 12345});
        model.render(pass);
        monitor.require_drained();
        const auto& observed = monitor.statistics();
        expect(events == model.system_statistics().core_cycles && observed.cycles == events,
               "observer excluded boot or timed external transfers");
        expect(observed.accepted == 1024 && observed.retired == 1024 && observed.committed == 1024 &&
               observed.color_writes == model.statistics().color_writes &&
               observed.depth_writes == model.statistics().depth_writes,
               "observer acceptance/retirement differs from real attachment traffic");
        expect(observed.max_consecutive_accepts == 1024 && observed.max_consecutive_commits == 1024 &&
               observed.same_cycle_accept_retire == 1023 && observed.max_inflight == 1,
               "dense untextured draw did not sustain its registered one-fragment cadence");
        expect(observed.stage_cycles[static_cast<unsigned>(FragmentStage::Transfer)] != 0 &&
               observed.stage_cycles[static_cast<unsigned>(FragmentStage::Idle)] != 0 &&
               model.system_statistics().last_pass_cycles > model.statistics().hardware_cycles,
               "common-clock observations omitted transfer/idle bubbles or whole-pass time");
        model.queue_present(1); model.vertical_blank();
        monitor.require_drained();
        expect(events == model.system_statistics().core_cycles && monitor.statistics().accepted == 1024 &&
               monitor.statistics().retired == 1024 && model.scanout_statistics().underflows == 0,
               "presentation replayed stale fragment flags or bypassed observed system clocks");
        model.set_cycle_observer({});
    });

    run_case("abort cancels an owned textured fragment through the same common-clock observer", [] {
        SystemModel model; boot(model); RawHost host(model);
        ThroughputMonitor monitor;
        model.set_cycle_observer([&](std::uint64_t clock, const FragmentCycle& event) { monitor.observe(event, clock); });
        Pass pass; auto state = overlay_state(); state.texture = pattern();
        pass.primitives.emplace_back(Sprite{{0, 0, 8, 8}, 0, 0, state, 0});
        const auto program = compile_pass(pass);
        host.send(program.bytes);
        wait_until(model, [&] { return monitor.inflight() != 0; }, 1000000);
        model.abort();
        monitor.require_drained();
        expect(monitor.statistics().accepted == 1 && monitor.statistics().cancelled == 1 &&
               monitor.statistics().color_writes == 0 && !model.color_valid(1) && !model.depth_valid(),
               "abort committed or lost its retained fragment instead of cancelling it");
        model.advance(100);
        expect(monitor.statistics().cancelled == 1, "idle recovery replayed a fragment cancellation");
        model.set_cycle_observer({});
    });

    for (const bool pin_reset : {false, true}) {
        run_case(pin_reset ? "host hard reset preserves observer time IDs and cancels an issued texture read" :
                            "public hard reset preserves observer time IDs and cancels an issued texture read", [=] {
            SystemModel model; boot(model); RawHost host(model);
            ThroughputMonitor monitor;
            std::uint64_t last_clock = 0, last_id = 0, events = 0, reset_events = 0;
            model.set_cycle_observer([&](std::uint64_t clock, const FragmentCycle& event) {
                expect(last_clock == 0 || clock == last_clock + 1, "hard reset broke monotonic observer time");
                last_clock = clock; ++events;
                if (event.stage == FragmentStage::Reset) ++reset_events;
                if (event.accepted) {
                    expect(event.accepted_fragment.id > last_id, "hard reset reused an observable fragment ID");
                    last_id = event.accepted_fragment.id;
                }
                monitor.observe(event, clock);
            });
            const auto launch = [&] {
                Pass pass; auto state = overlay_state(); state.texture = pattern();
                pass.primitives.emplace_back(Sprite{{0, 0, 8, 8}, 0, 0, state, 0});
                const auto program = compile_pass(pass, model.system_statistics().completed_sequence + 1);
                host.send(program.bytes);
                wait_until(model, [&] {
                    const auto& texture = model.memory_statistics().clients[static_cast<unsigned>(MemoryClient::Texture)];
                    return monitor.inflight() != 0 && texture.requests != texture.completed;
                }, 1000000);
            };
            launch();
            const auto prior_events = events, prior_resets = reset_events;
            const auto prior_id = last_id;
            const auto prior_epoch = model.system_statistics().epoch;
            if (pin_reset) { HostPins pins; pins.reset = true; model.tick(pins); model.tick(); }
            else model.reset();
            monitor.require_drained();
            expect(events == prior_events + (pin_reset ? 2 : 1) && reset_events == prior_resets + 1 &&
                   monitor.statistics().cancelled == 1 && model.system_statistics().epoch == prior_epoch + 1 &&
                   model.system_statistics().core_cycles == 1 && model.memory_statistics().cycles == 1 && !model.ready(),
                   "hard reset lost its observer, failed cancellation, or skipped its single initialization edge");
            boot(model);
            launch();
            expect(last_id > prior_id, "new hard-reset epoch did not advance global fragment IDs");
            model.abort(); monitor.require_drained();
            expect(monitor.statistics().accepted == 2 && monitor.statistics().cancelled == 2,
                   "observer did not retain balanced accounting after reset and fresh traffic");
            model.set_cycle_observer({});
        });
    }
}

void recovery_tests() {
    run_case("malformed packet leaves sideband status and fresh-epoch recovery available", [] {
        SystemModel model; boot(model); RawHost host(model);
        auto packet = encode_packet(Opcode::GetCaps, 1); packet[0] = 'X';
        host.send(packet);
        for (unsigned edge = 0; edge < 100 && !model.faulted(); ++edge) model.tick();
        expect(model.faulted(), "malformed packet was accepted for execution");
        const auto status = host.status();
        expect((status[1] & 0x80u) != 0 && status[56] == static_cast<unsigned>(HostFault::Magic) &&
               status_u32(status, 6) == 0 && model.system_statistics().commands_completed == 0,
               "parser fault blocked coherent status or retired a request");
        const auto epoch = model.system_statistics().epoch;
        model.abort();
        expect(!model.faulted() && model.system_statistics().epoch == epoch + 1 && model.front_id() == 0,
               "out-of-band abort did not recover a fresh parser epoch");
        host.send(encode_packet(Opcode::GetCaps, 1));
        const auto caps = host.reply(Opcode::GetCaps, 1);
        expect(caps.size() == 92 && caps[30] == 8 && read_u32(caps, 32) == memory_bytes &&
               read_u32(caps, 44) == primitive_record_bytes, "recovered capabilities lost the system ABI");
    });
    run_case("partial packets never execute and abort discards every old fragment", [] {
        SystemModel model; boot(model); RawHost host(model);
        auto payload = scalar(heap_base); append(payload, scalar(4)); payload.insert(payload.end(), {1, 2, 3, 4});
        auto packet = encode_packet(Opcode::WriteHeap, 1, payload); packet.resize(18);
        host.send(packet); model.advance(1000);
        expect(model.system_statistics().uploaded_bytes == 0 && model.system_statistics().commands_completed == 0 &&
               !model.faulted(), "partially received write executed");
        model.abort();
        payload = scalar(heap_base); append(payload, scalar(4)); payload.insert(payload.end(), {9, 8, 7, 6});
        auto bytes = encode_packet(Opcode::WriteHeap, 1, payload);
        append(bytes, encode_packet(Opcode::Fence, 2, scalar(0x55aa))); host.send(bytes);
        expect(read_u32(host.reply(Opcode::Fence, 2), 24) == 0x55aa, "new epoch fence was not returned");
        expect(host.read_memory(3, heap_base, 4, 0x1234) == std::vector<std::uint8_t>({9, 8, 7, 6}),
               "late old packet fragments entered the recovered upload stream");
    });
    run_case("status reads bypass a PRESENT waiting for an actual vertical blank", [] {
        SystemModel model; model.render(clear_pass(1, 0x4a69)); RawHost host(model);
        // Begin well inside active video, keeping this bounded status snapshot
        // entirely before the next vertical-blank presentation opportunity.
        while (model.system_statistics().core_cycles % frame_cycles != 4000) model.tick();
        const auto sequence = model.system_statistics().completed_sequence + 1;
        auto payload = scalar(1); append(payload, scalar(0xabcd1234));
        host.send(encode_packet(Opcode::Present, sequence, payload));
        model.advance(200);
        expect(model.presentation_pending() && model.front_id() == 0, "PRESENT skipped its vertical-blank wait");
        const auto before_reads = model.host_statistics().status_reads;
        const auto snapshot = host.status();
        expect((snapshot[1] & 6u) == 6u && snapshot[14] == 0 &&
               model.host_statistics().status_reads == before_reads + 63 && model.front_id() == 0,
               "in-order presentation blocked independent status access");
        const auto reply = host.reply(Opcode::Present, sequence);
        expect(model.front_id() == 1 && read_u32(reply, 24) == 0xabcd1234 &&
               model.system_statistics().presentations_completed == 1 && model.scanout_statistics().underflows == 0,
               "vblank did not retire the queued presentation");
    });
    run_case("abort drains issued memory while preserving front and invalidating interrupted attachments", [] {
        SystemModel model; model.render(clear_pass(1, 0x25a5)); model.queue_present(1); model.vertical_blank();
        const auto front = model.front_color(); const auto old_epoch = model.system_statistics().epoch;
        Pass interrupted = clear_pass(0, 0xaaaa);
        auto state = overlay_state({240, 90, 40}); state.depth_write = true;
        interrupted.primitives.emplace_back(Rect{{0, 0, 640, 480}, state, 0x123456});
        const auto first = model.system_statistics().completed_sequence + 1;
        const auto program = compile_pass(interrupted, first);
        RawHost host(model); host.send(program.bytes);
        wait_until(model, [&] {
            const auto& tile = model.memory_statistics().clients[static_cast<unsigned>(MemoryClient::Tile)];
            return !model.color_valid(0) && tile.requests != tile.completed;
        }, 1000000);
        model.abort();
        expect(model.ready() && !model.faulted() && model.system_statistics().epoch == old_epoch + 1 &&
               model.front_id() == 1 && model.color_valid(1) && !model.color_valid(0) && !model.depth_valid(),
               "abort lost ownership or advertised interrupted attachments as initialized");
        model.advance(frame_cycles * 2);
        expect(model.front_color() == front && model.display_color() == front && model.scanout_statistics().underflows == 0,
               "abort changed or interrupted the independent front display");
        Pass invalid_load = clear_pass(0); invalid_load.color_load = invalid_load.depth_load = LoadOp::Load;
        expect_throws([&] { model.render(invalid_load); }, "LOAD accepted invalid interrupted contents");
        model.abort();
        model.render(clear_pass(0, 0x6b4d));
        expect(model.color_valid(0) && model.depth_valid() &&
               std::all_of(model.color(0).begin(), model.color(0).end(), [](auto pixel) { return pixel == 0x6b4d; }) &&
               std::all_of(model.depth().begin(), model.depth().end(), [](auto depth) { return depth == depth_max; }) &&
               model.front_color() == front, "CLEAR/CLEAR did not recover attachments after abort");
    });
    run_case("wire-corrupted fetched record faults before a later fence can complete", [] {
        Pass pass; pass.primitives.emplace_back(Rect{{8, 8, 16, 16}, overlay_state({200, 80, 30}), 0});
        auto program = compile_pass(pass);
        // First allocation is the untextured primitive record. Corrupt its
        // reserved byte in WRITE_HEAP traffic, rather than editing a C++ draw.
        bool changed = false;
        for (std::size_t cursor = 0; cursor < program.bytes.size();) {
            const auto length = read_u32(program.bytes, cursor + 8);
            if (read_u16(program.bytes, cursor + 6) == static_cast<unsigned>(Opcode::WriteHeap)) {
                const auto address = read_u32(program.bytes, cursor + packet_header_bytes);
                const auto size = read_u32(program.bytes, cursor + packet_header_bytes + 4);
                if (address <= heap_base + 76 && heap_base + 76 < static_cast<std::uint64_t>(address) + size) {
                    program.bytes[cursor + packet_header_bytes + 8 + heap_base + 76 - address] = 1;
                    changed = true;
                }
            }
            cursor += packet_header_bytes + length;
        }
        expect(changed, "test did not modify uploaded record bytes");
        SystemModel model; boot(model); RawHost host(model); host.send(program.bytes);
        for (std::uint64_t edge = 0; edge < 1000000 && !model.faulted(); ++edge) model.tick();
        expect(model.faulted() && !model.color_valid(1) && !model.depth_valid() && model.color_valid(0) &&
               model.system_statistics().passes_completed == 0 && model.system_statistics().completed_fence_token == 0,
               "malformed fetched record executed or retired a later completion fence");
        expect((host.status_byte(1) & 8u) != 0,
               "execution fault was not available through independent status");
    });
    run_case("out-of-band hard reset restarts timed initialization and clears previous back contents", [] {
        auto image = pattern(); SystemModel model;
        model.copy_image(*image, 1, {0, 0}, {0, 0}, 8, 8);
        expect(model.color(1)[1] != 0, "reset test never wrote back-buffer contents");
        const auto epoch = model.system_statistics().epoch;
        HostPins reset; reset.reset = true; model.tick(reset); model.tick();
        expect(model.system_statistics().epoch == epoch + 1 && !model.ready() && !model.color_valid(0) &&
               !model.color_valid(1) && !model.depth_valid(), "hard-reset pin did not start a fresh uninitialized epoch");
        boot(model); RawHost host(model);
        const auto color = host.read_memory(1, color_buffer_base(1), 16, 1);
        expect(std::all_of(color.begin(), color.end(), [](auto byte) { return byte == 0; }),
               "hard reset preserved stale back-buffer bytes");
        expect(read_u32(host.read_memory(2, depth_buffer_base, 4, 2), 0) == depth_max &&
               model.memory_statistics().clients[static_cast<unsigned>(MemoryClient::Tile)].bytes_written == heap_base,
               "hard reset skipped timed color/depth reinitialization");
    });
}

} // namespace

int main() {
    boot_and_io_tests(); rendering_tests(); copy_tests(); clock_observer_tests(); recovery_tests();
    std::cout << cases - failures << '/' << cases << " system integration cases passed\n";
    return failures == 0 ? 0 : 1;
}
