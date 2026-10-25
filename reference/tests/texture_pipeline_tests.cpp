#include "nano_gpu/hw/texture.hpp"
#include "nano_gpu/hw/sdram.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace nano_gpu;
using namespace nano_gpu::hw;

namespace {
unsigned cases = 0, failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void throws(Function&& operation, const char* message) {
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    expect(rejected, message);
}
template<class Function> void run(const char* name, Function&& operation) {
    ++cases;
    try { operation(); }
    catch (const std::exception& error) {
        ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

std::shared_ptr<Image> pattern(int width, int height, int pitch = 0) {
    auto image = std::make_shared<Image>(width, height, pitch);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image->at(x, y) = static_cast<std::uint16_t>((y * 977 + x * 73 + 0x1234) & 0xffff);
    return image;
}

struct StreamRun {
    std::vector<unsigned> inputs, outputs;
    unsigned edges = 0, disabled = 0, max_inflight = 0;
};
StreamRun drive(TextureUnit& unit, const Image& image,
                const std::vector<std::pair<int, int>>& coordinates,
                bool stalls = false, bool pressure = false, SdramController* memory = nullptr) {
    StreamRun result;
    unsigned sent = 0, received = 0;
    while (received != coordinates.size() || unit.busy()) {
        if (++result.edges > 2000000) throw std::runtime_error("texture pipeline failed to drain");
        const bool enable = !stalls || (result.edges % 5 != 1 && result.edges % 7 != 3);
        if (!enable) ++result.disabled;
        if (enable && unit.output_valid() && (!pressure || result.edges % 11 >= 5)) {
            expect(received < coordinates.size(), "texture produced an unrequested output");
            const auto [x, y] = coordinates[received];
            expect(unit.output() == image.at(x, y), "texture output reordered or returned a stale texel");
            expect(unit.pop(), "valid texture output could not reserve consumption");
            ++received; result.outputs.push_back(result.edges);
        }
        if (enable && sent < coordinates.size() && unit.input_ready()) {
            const auto [x, y] = coordinates[sent];
            expect(unit.push(x, y), "ready texture input was rejected");
            ++sent; result.inputs.push_back(result.edges);
        }
        result.max_inflight = std::max(result.max_inflight, unit.inflight());
        unit.tick(enable);
        if (memory) memory->tick();
    }
    expect(sent == coordinates.size(), "texture drained before every input was accepted");
    expect(unit.inflight() == 0 && !unit.output_valid(), "texture retained stale output after drain");
    expect(result.max_inflight <= TextureUnit::streaming_capacity, "texture exceeded bounded context capacity");
    return result;
}

void boot(SdramController& memory) {
    for (unsigned guard = 0; !memory.initialized(); ++guard) {
        expect(guard < 20000, "SDRAM initialization hung"); memory.tick();
    }
}
void upload(SdramController& memory, std::uint32_t base, const Image& image) {
    const auto bytes = static_cast<unsigned>(image.pitch_bytes() * image.height());
    expect((bytes & 3u) == 0, "external texture upload must contain complete words");
    for (unsigned offset = 0; offset < bytes;) {
        MemoryRequest request; request.write = true; request.address = base + offset;
        request.words = std::min(16u, (bytes - offset) / 4u);
        for (unsigned word = 0; word < request.words; ++word) {
            for (unsigned lane = 0; lane < 2; ++lane) {
                const auto at = offset + 4 * word + 2 * lane;
                const auto y = at / static_cast<unsigned>(image.pitch_bytes());
                const auto x = at % static_cast<unsigned>(image.pitch_bytes()) / 2;
                const auto value = x < static_cast<unsigned>(image.width()) ?
                    image.at(static_cast<int>(x), static_cast<int>(y)) : std::uint16_t{0};
                request.data[word] |= static_cast<std::uint32_t>(value) << (16 * lane);
            }
            request.masks[word] = 0xf;
        }
        expect(memory.submit(MemoryClient::Host, request), "test upload slot was not free");
        unsigned guard = 0;
        while (!memory.response_ready(MemoryClient::Host)) {
            expect(++guard < 1000, "test upload failed to complete"); memory.tick();
        }
        (void)memory.take_response(MemoryClient::Host); offset += 4 * request.words;
    }
}

void local_tests() {
    run("serial oracle retains its original hit and miss schedules", [] {
        const auto image = pattern(16, 16);
        TextureUnit unit; unit.bind(image);
        const auto first = unit.statistics().cycles;
        unit.start(0, 0);
        while (unit.busy()) unit.tick();
        expect(unit.result() == image->at(0, 0) && unit.statistics().cycles - first == 34,
               "streaming implementation changed the serial miss oracle");
        const auto second = unit.statistics().cycles;
        unit.start(1, 0);
        while (unit.busy()) unit.tick();
        expect(unit.result() == image->at(1, 0) && unit.statistics().cycles - second == 14,
               "streaming implementation changed the serial hit oracle");
    });
    run("warm cache accepts and emits 1024 texels on consecutive edges", [] {
        const auto image = pattern(64, 64);
        TextureUnit unit; unit.pipeline_mode(true); unit.bind(image);
        (void)drive(unit, *image, {{0, 0}});
        std::vector<std::pair<int, int>> coordinates;
        for (int i = 0; i < 1024; ++i) coordinates.emplace_back(i & 15, 0);
        const auto before = unit.statistics();
        const auto observed = drive(unit, *image, coordinates);
        expect(observed.inputs.size() == 1024 && observed.outputs.size() == 1024,
               "sustained stream lost input/output events");
        for (unsigned i = 0; i < 1024; ++i) {
            expect(observed.inputs[i] == i + 1, "cache-hit input initiation interval exceeded one");
            expect(observed.outputs[i] - observed.inputs[i] == TextureUnit::pipeline_hit_edges,
                   "cache-hit output skipped or added a register boundary");
            if (i != 0) expect(observed.outputs[i] == observed.outputs[i-1] + 1,
                               "cache-hit output initiation interval exceeded one");
        }
        expect(unit.statistics().hits - before.hits == 1024 && unit.statistics().misses == before.misses,
               "warm stream refilled memory or miscounted cache requests");
        expect(unit.statistics().pipeline_inputs - before.pipeline_inputs == 1024 &&
               unit.statistics().pipeline_outputs - before.pipeline_outputs == 1024,
               "streaming handshakes were counted before their clock edges");
    });
    run("same-index misses preserve older results and younger request order", [] {
        const auto image = pattern(64, 64);
        TextureUnit unit; unit.pipeline_mode(true); unit.bind(image);
        std::vector<std::pair<int, int>> coordinates;
        for (int i = 0; i < 100; ++i) {
            coordinates.emplace_back(i & 15, (i % 3) * 16);
            coordinates.emplace_back((i + 1) & 15, (i % 3) * 16);
        }
        (void)drive(unit, *image, coordinates);
        expect(unit.statistics().misses > 50 && unit.statistics().hits != 0,
               "conflict test did not exercise both misses and hits");
        expect(unit.statistics().hits + unit.statistics().misses == coordinates.size(),
               "replayed miss was counted as a second request");
    });
    run("clock stalls and downstream backpressure conserve pipelined texels", [] {
        const auto image = pattern(64, 64, 144);
        TextureUnit unit; unit.pipeline_mode(true); unit.bind(image);
        std::vector<std::pair<int, int>> coordinates;
        for (int i = 0; i < 400; ++i) coordinates.emplace_back((i * 19) & 63, (i * 7) & 63);
        const auto observed = drive(unit, *image, coordinates, true, true);
        expect(observed.disabled != 0 && unit.statistics().pipeline_backpressure_cycles != 0,
               "test omitted disabled or backpressured clocks");
        expect(unit.statistics().stalls == observed.disabled,
               "disabled edges were omitted from elapsed texture time");
    });
    run("pitch high partial product and sparse padded rows remain exact", [] {
        const auto image = pattern(4, 4, (1 << 19) + 12);
        TextureUnit unit; unit.pipeline_mode(true); unit.bind(image);
        std::vector<std::pair<int, int>> coordinates;
        for (int i = 0; i < 64; ++i) coordinates.emplace_back(i & 3, (i >> 2) & 3);
        (void)drive(unit, *image, coordinates, true, true);
    });
    run("reset cancels local reservations and preserves static pipeline selection", [] {
        const auto image = pattern(16, 16);
        TextureUnit unit; unit.pipeline_mode(true); unit.bind(image);
        expect(unit.push(1, 1), "pipeline did not accept initial reservation");
        expect(!unit.push(2, 2), "two inputs reserved the same physical edge");
        throws([&] { unit.bind(image); }, "binding replaced an outstanding request");
        throws([&] { unit.pipeline_mode(false); }, "configuration changed with a retained request");
        unit.tick(false);
        expect(unit.inflight() == 1 && unit.statistics().pipeline_inputs == 0,
               "disabled edge consumed a staged producer");
        unit.reset();
        expect(unit.pipeline_mode() && !unit.busy() && !unit.input_ready() && !unit.output_valid(),
               "reset preserved stale requests or lost static configuration");
        unit.bind(image); (void)drive(unit, *image, {{7, 9}});
        unit.scratch_write(33, 0x9d73); unit.scratch_read_request(33);
        expect(unit.scratch_read_result() == 0x9d73, "copy-row scratch did not share the selected cache bank");
        throws([&] { unit.start(0, 0); }, "serial API bypassed configured streaming handshakes");
    });
}

void external_tests() {
    run("streaming cache misses use synchronous owned SDRAM slots", [] {
        const auto image = pattern(8, 8, 24);
        SdramController memory; boot(memory); upload(memory, heap_base, *image);
        TextureUnit unit; unit.pipeline_mode(true);
        unit.bind_external({heap_base, 8, 8, 24}, memory);
        std::vector<std::pair<int, int>> coordinates;
        for (int i = 0; i < 256; ++i) coordinates.emplace_back((i * 3) & 7, (i * 5) & 7);
        const auto before = memory.statistics();
        (void)drive(unit, *image, coordinates, true, true, &memory);
        const auto& after = memory.statistics();
        expect(after.legacy_submissions == before.legacy_submissions &&
               after.legacy_response_reads == before.legacy_response_reads,
               "texture used a whole-array memory adapter");
        expect(after.slot_read_words > before.slot_read_words && !unit.external_memory_pending(),
               "external cache line did not cross its native synchronous slot port");
        expect(after.uninitialized_read_words == 0 && after.refresh_late_cycles == 0,
               "streamed texture crossed allocation/refresh bounds");
    });
    run("short external lines pad their unused tail without extra SDRAM reads", [] {
        const auto image = pattern(1, 1, 4);
        SdramController memory; boot(memory); upload(memory, heap_base, *image);
        TextureUnit unit; unit.pipeline_mode(true);
        unit.bind_external({heap_base, 1, 1, 4}, memory);
        const auto before = memory.statistics().clients[static_cast<unsigned>(MemoryClient::Texture)];
        (void)drive(unit, *image, std::vector<std::pair<int, int>>(32, {0, 0}), true, true, &memory);
        const auto& after = memory.statistics().clients[static_cast<unsigned>(MemoryClient::Texture)];
        expect(after.requests - before.requests == 1 && after.bytes_read - before.bytes_read == 4 &&
               unit.statistics().refill_words == 16,
               "short cache line read beyond its allocation or skipped tail initialization");
    });
    run("external reset requires response ownership to drain", [] {
        const auto image = pattern(4, 4);
        SdramController memory; boot(memory); upload(memory, heap_base, *image);
        TextureUnit unit; unit.pipeline_mode(true);
        unit.bind_external({heap_base, 4, 4, static_cast<unsigned>(image->pitch_bytes())}, memory);
        expect(unit.push(0, 0), "initial external input was rejected");
        unsigned guard = 0;
        while (!unit.external_memory_pending()) {
            expect(++guard < 100, "miss failed to issue an external request"); unit.tick(); memory.tick();
        }
        throws([&] { unit.reset(); }, "reset discarded an owned texture SDRAM response");
        while (unit.external_memory_pending()) {
            expect(++guard < 1000, "external response failed to drain"); unit.tick(); memory.tick();
        }
        unit.reset();
        expect(unit.pipeline_mode() && !unit.busy(), "recovery retained stale pipeline contexts");
        unit.bind_external({heap_base, 4, 4, static_cast<unsigned>(image->pitch_bytes())}, memory);
        (void)drive(unit, *image, {{3, 2}}, false, false, &memory);
    });
}
} // namespace

int main() {
    local_tests(); external_tests();
    std::cout << cases - failures << '/' << cases << " streaming texture cases passed\n";
    return failures == 0 ? 0 : 1;
}
