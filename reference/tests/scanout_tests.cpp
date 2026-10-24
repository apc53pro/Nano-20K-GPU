#include "nano_gpu/hw/scanout.hpp"
#include "nano_gpu/hw/sdram.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace nano_gpu;
using namespace nano_gpu::hw;

unsigned cases = 0, passed = 0;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Action>
void expect_throw(Action&& action, const char* message) {
    try { action(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

template<class Action>
void run_case(const char* name, Action&& action) {
    ++cases;
    try { action(); ++passed; }
    catch (const std::exception& error) {
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

struct Rig {
    explicit Rig(ScanoutConfig video = {}, SdramConfig ram = {}) : memory(ram), scanout(video) {}
    SdramController memory;
    Scanout scanout;
    void tick() { scanout.tick(memory); memory.tick(); }
    template<class Predicate>
    void until(Predicate&& predicate, std::uint64_t maximum = 5000000) {
        for (std::uint64_t edge = 0; edge < maximum; ++edge) {
            if (predicate()) return;
            tick();
        }
        throw std::runtime_error("bounded scanout wait exceeded its deadline");
    }
    void initialize() { until([&] { return memory.initialized(); }); }
    template<class Pixel>
    void write_frame(int id, Pixel&& pixel) {
        constexpr unsigned pixels = screen_width * screen_height;
        for (unsigned offset = 0; offset < pixels; offset += memory_burst_words * 2) {
            MemoryRequest request;
            request.write = true;
            request.address = static_cast<std::uint32_t>(id) * pixels * 2u + offset * 2u;
            request.words = memory_burst_words;
            for (unsigned word = 0; word < memory_burst_words; ++word) {
                const auto first = offset + word * 2;
                request.data[word] = static_cast<std::uint32_t>(pixel(first)) |
                                     (static_cast<std::uint32_t>(pixel(first + 1)) << 16);
                request.masks[word] = 0xf;
            }
            expect(memory.submit(MemoryClient::Host, request), "host write slot unexpectedly occupied");
            until([&] { return memory.response_ready(MemoryClient::Host); });
            memory.take_response(MemoryClient::Host);
        }
    }
    void next_blank() {
        tick();
        until([&] { return scanout.vertical_blank_pulse(); });
    }
};

std::uint16_t pattern(unsigned pixel) {
    const unsigned x = pixel % screen_width;
    const unsigned y = pixel / screen_width;
    return static_cast<std::uint16_t>((x * 173u) ^ (y * 997u) ^ 0x5a35u);
}

void expect_pattern(const Scanout& scanout) {
    const auto& frame = scanout.completed_frame();
    expect(frame.size() == static_cast<std::size_t>(screen_width) * screen_height,
           "diagnostic frame has incorrect size");
    for (unsigned pixel = 0; pixel < frame.size(); ++pixel)
        if (frame[pixel] != pattern(pixel)) throw std::runtime_error("scanout pixel order/content changed");
}

} // namespace

int main() {
    run_case("standard raster timing is exactly 60 Hz while memory is unavailable", [] {
        Rig rig;
        constexpr std::uint64_t clocks_per_frame = 4u * 800u * 525u;
        constexpr std::uint64_t active_per_frame = 640u * 480u;
        std::uint64_t hsync_low = 0, vsync_low = 0, blanks = 0, wraps = 0;
        for (std::uint64_t edge = 0; edge < clocks_per_frame; ++edge) {
            rig.tick();
            if (!rig.scanout.horizontal_sync()) ++hsync_low;
            if (!rig.scanout.vertical_sync()) ++vsync_low;
            if (rig.scanout.vertical_blank_pulse()) ++blanks;
            if (rig.scanout.frame_pulse()) ++wraps;
        }
        expect(rig.memory.config().core_hz / clocks_per_frame == 60, "default model is not 60 Hz");
        expect(rig.scanout.frame_count() == 1 && wraps == 1 && blanks == 1,
               "raster frame/blank pulses have wrong periods");
        expect(hsync_low == 96u * 525u * 4u && vsync_low == 2u * 800u * 4u,
               "negative sync widths differ from the selected timing");
        const auto& counters = rig.scanout.statistics();
        expect(counters.pixel_ticks == 800u * 525u && counters.active_pixels == active_per_frame &&
               counters.startup_black_pixels == active_per_frame && counters.underflows == 0,
               "startup must keep valid raster timing without underflow");
        expect(counters.read_requests == 0 && rig.scanout.completed_frame_front_id() == -1,
               "scanout read uninitialized attachments");
        expect(std::all_of(rig.scanout.completed_frame().begin(), rig.scanout.completed_frame().end(),
                           [](auto value) { return value == 0; }), "startup frame was not black");
    });

    run_case("shared memory refills the bounded synchronous FIFO in exact pixel order", [] {
        Rig rig;
        rig.initialize();
        rig.write_frame(0, pattern);
        const auto legacy_submissions = rig.memory.statistics().legacy_submissions;
        const auto legacy_reads = rig.memory.statistics().legacy_response_reads;
        rig.scanout.set_memory_ready(true);
        rig.next_blank(); // Arm and prefetch in vertical blank.
        rig.next_blank(); // Observe the first full active frame.
        expect(rig.scanout.completed_frame_front_id() == 0, "completed front ID was not retained");
        expect_pattern(rig.scanout);
        const auto& counters = rig.scanout.statistics();
        expect(counters.underflows == 0 && counters.displayed_pixels == 640u * 480u,
               "default memory service did not sustain scanout");
        expect(counters.fifo_high_water <= rig.scanout.config().fifo_pixels &&
               counters.fifo_high_water > 1024, "FIFO capacity/watermark accounting is incorrect");
        const auto& client = rig.memory.statistics().clients[static_cast<unsigned>(MemoryClient::Scanout)];
        expect(client.bytes_read >= 640u * 480u * 2u && counters.read_requests >= 9600,
               "pixels bypassed shared SDRAM requests");
        expect(rig.memory.statistics().legacy_submissions == legacy_submissions &&
               rig.memory.statistics().legacy_response_reads == legacy_reads &&
               rig.memory.statistics().slot_read_words >= 640u * 480u / 2u,
               "scanout bypassed synchronous native response-slot reads");
    });

    run_case("slow row activation produces observable display underflow with a small FIFO", [] {
        ScanoutConfig video;
        video.fifo_pixels = 64;
        SdramConfig ram;
        ram.t_rcd = 300; // Deliberately pessimistic configurable service latency.
        Rig rig(video, ram);
        rig.initialize();
        rig.write_frame(0, pattern);
        rig.scanout.set_memory_ready(true);
        rig.next_blank();
        rig.next_blank();
        expect(rig.scanout.statistics().underflows != 0 &&
               rig.scanout.statistics().underflow_pixels != 0,
               "scanout ignored real memory response latency");
        expect(rig.scanout.frame_count() != 0, "display timing stopped with the memory client");
        expect(rig.scanout.statistics().fifo_high_water <= 64, "stress FIFO exceeded selected bound");
    });

    run_case("scanout and competing render clients share SDRAM without starvation", [] {
        Rig rig;
        rig.initialize();
        rig.write_frame(0, pattern);
        rig.scanout.set_memory_ready(true);
        rig.next_blank();
        const auto target = rig.scanout.statistics().completed_frames + 1;
        const std::array<MemoryClient, 4> clients{{MemoryClient::Command, MemoryClient::Tile,
                                                 MemoryClient::Texture, MemoryClient::Copy}};
        std::array<unsigned, 4> completions{};
        for (unsigned edge = 0; edge < 5000000 &&
             rig.scanout.statistics().completed_frames < target; ++edge) {
            for (unsigned client = 0; client < clients.size(); ++client) {
                if (rig.memory.response_ready(clients[client])) {
                    rig.memory.take_response(clients[client]);
                    ++completions[client];
                }
                MemoryRequest request;
                request.words = memory_burst_words;
                // Separate heap rows create real row/bank competition. The
                // tile writer never touches the displayed attachments.
                request.write = clients[client] == MemoryClient::Tile;
                request.address = request.write ? 0x258000u +
                    ((completions[client] & 255u) * 4096u) :
                    ((completions[client] * 64u + client * 1024u) % 614400u) & ~63u;
                request.data.fill(0x12345678u);
                request.masks.fill(0xf);
                rig.memory.submit(clients[client], request);
            }
            rig.tick();
        }
        expect(rig.scanout.statistics().completed_frames == target,
               "competing memory clients stopped the independent raster");
        expect_pattern(rig.scanout);
        expect(rig.scanout.statistics().underflows == 0,
               "selected arbitration could not sustain the default display FIFO");
        for (const auto count : completions)
            expect(count > 100, "scanout priority starved another active memory client");
        expect(rig.memory.statistics().refreshes > 2 &&
               rig.memory.statistics().refresh_late_cycles == 0,
               "shared traffic blocked mandatory refresh");
    });

    run_case("withheld memory service blanks a frame and recovers without pixel displacement", [] {
        Rig rig;
        rig.initialize();
        rig.write_frame(0, pattern);
        rig.scanout.set_memory_ready(true);
        rig.next_blank();
        rig.until([&] { return rig.scanout.frame_pulse(); });
        const auto before = rig.scanout.statistics().underflows;
        // Deliberately withhold memory-controller service while the independent
        // pixel scheduler keeps advancing; this is a failure-injection test.
        for (unsigned edge = 0; edge < 12000; ++edge) rig.scanout.tick(rig.memory);
        expect(rig.scanout.statistics().underflows == before + 1,
               "missing memory service did not produce exactly one frame-underflow event");
        rig.next_blank(); // Finish damaged frame; restart the fetch sequence.
        rig.next_blank(); // The following full frame must recover pixel alignment.
        expect_pattern(rig.scanout);
        expect(rig.scanout.statistics().underflows == before + 1,
               "restored memory service did not recover at vertical blank");
    });

    run_case("vertical blank swaps discard old prefetch and never mix physical buffers", [] {
        Rig rig;
        rig.initialize();
        rig.write_frame(0, [](unsigned) { return std::uint16_t{0x07e0}; });
        rig.write_frame(1, [](unsigned) { return std::uint16_t{0xf81f}; });
        rig.scanout.set_memory_ready(true);
        expect_throw([&] { rig.scanout.set_front(1); }, "live front changed outside vertical blank");
        rig.next_blank();
        rig.next_blank();
        expect(std::all_of(rig.scanout.completed_frame().begin(), rig.scanout.completed_frame().end(),
                           [](auto value) { return value == 0x07e0; }), "old front frame was mixed");
        rig.scanout.set_front(1);
        rig.next_blank();
        expect(rig.scanout.completed_frame_front_id() == 1 &&
               std::all_of(rig.scanout.completed_frame().begin(), rig.scanout.completed_frame().end(),
                           [](auto value) { return value == 0xf81f; }), "swap exposed stale old-front pixels");
        expect(rig.scanout.statistics().framebuffer_swaps == 1 &&
               rig.scanout.statistics().stale_responses != 0 && rig.scanout.statistics().underflows == 0,
               "swap did not invalidate pending reads safely");
    });

    run_case("invalid timing, capacity, and buffer IDs are rejected", [] {
        ScanoutConfig config;
        config.pixel_divider = 0;
        expect_throw([&] { Scanout invalid(config); }, "zero pixel divider accepted");
        config = {};
        config.fifo_pixels = 4096;
        expect_throw([&] { Scanout invalid(config); }, "unallocated FIFO memory accepted");
        config.fifo_pixels = 100;
        expect_throw([&] { Scanout invalid(config); }, "non-power-of-two FIFO accepted");
        config = {};
        config.vertical_sync = 0;
        expect_throw([&] { Scanout invalid(config); }, "missing sync interval accepted");
        Scanout valid;
        expect_throw([&] { valid.set_front(2); }, "invalid physical color ID accepted");
        expect(valid.front_id() == 0, "rejected buffer selection changed the front");
    });

    std::cout << passed << '/' << cases << " scanout cases passed\n";
    return passed == cases ? 0 : 1;
}
