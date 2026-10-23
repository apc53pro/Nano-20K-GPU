#include "nano_gpu/hw/sdram.hpp"
#include "nano_gpu/hw/system_copy.hpp"
#include "nano_gpu/hw/tile_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nano_gpu;
using namespace nano_gpu::hw;

namespace {

unsigned cases = 0, failures = 0;

void expect(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
template<class Function>
void throws(Function&& function, const std::string& message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    expect(rejected, message);
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

void boot(SdramController& memory) {
    for (unsigned guard = 0; !memory.initialized(); ++guard) {
        expect(guard < 50000, "SDRAM initialization hung");
        memory.tick();
    }
}

void upload(SdramController& memory, std::uint32_t address, const std::vector<std::uint32_t>& words) {
    for (std::size_t first = 0; first < words.size();) {
        MemoryRequest request;
        request.write = true;
        request.address = address + static_cast<std::uint32_t>(first * 4);
        request.words = static_cast<unsigned>(std::min<std::size_t>(memory_burst_words, words.size() - first));
        for (unsigned i = 0; i < request.words; ++i) {
            request.data[i] = words[first + i];
            request.masks[i] = 0xf;
        }
        expect(memory.submit(MemoryClient::Host, request), "test upload client unexpectedly busy");
        for (unsigned guard = 0; !memory.response_ready(MemoryClient::Host); ++guard) {
            expect(guard < 10000, "test upload hung");
            memory.tick();
        }
        memory.take_response(MemoryClient::Host);
        first += request.words;
    }
}

using Pixels = std::vector<std::uint16_t>;
Pixels blank() { return Pixels(screen_width * screen_height, 0xcdcd); }
std::uint16_t pattern(unsigned x, unsigned y) {
    return static_cast<std::uint16_t>(0x1234u + 17u * x + 113u * y);
}

Pixels seed_frame(SdramController& memory, unsigned id, unsigned width = 80, unsigned height = 80) {
    expect((width & 1u) == 0 && width <= screen_width && height <= screen_height, "invalid test seed");
    auto pixels = blank();
    for (unsigned y = 0; y < height; ++y) {
        std::vector<std::uint32_t> row(width / 2);
        for (unsigned x = 0; x < width; ++x) {
            const auto value = pattern(x, y);
            pixels[y * screen_width + x] = value;
            row[x / 2] |= static_cast<std::uint32_t>(value) << ((x & 1u) * 16);
        }
        upload(memory, color_buffer_base(id) + y * screen_width * 2u, row);
    }
    return pixels;
}

Pixels seed_image(SdramController& memory, const TextureDescriptor& image) {
    std::vector<std::uint32_t> storage(image.pitch_bytes * image.height / 4, 0xa5a5a5a5);
    Pixels pixels(image.width * image.height);
    for (unsigned y = 0; y < image.height; ++y) for (unsigned x = 0; x < image.width; ++x) {
        const auto value = pattern(x, y);
        pixels[y * image.width + x] = value;
        const auto half = y * (image.pitch_bytes / 2) + x;
        const auto shift = (half & 1u) * 16;
        storage[half / 2] = (storage[half / 2] & ~(0xffffu << shift)) |
                            (static_cast<std::uint32_t>(value) << shift);
    }
    upload(memory, image.base, storage);
    return pixels;
}

void expected_copy(Pixels& destination, const Pixels& source, unsigned source_width,
                   unsigned source_height, const CopyCommand& command) {
    // Independent snapshot oracle: iterate the visible destination, deriving
    // source coordinates from the original command, without using copy clips.
    const auto snapshot = source;
    for (int y = 0; y < screen_height; ++y) for (int x = 0; x < screen_width; ++x) {
        const auto offset_x = static_cast<std::int64_t>(x) - command.destination_origin.x;
        const auto offset_y = static_cast<std::int64_t>(y) - command.destination_origin.y;
        if (offset_x < 0 || offset_y < 0 || offset_x >= command.width || offset_y >= command.height) continue;
        const auto source_x = static_cast<std::int64_t>(command.source_origin.x) + offset_x;
        const auto source_y = static_cast<std::int64_t>(command.source_origin.y) + offset_y;
        if (source_x < 0 || source_y < 0 || source_x >= source_width || source_y >= source_height) continue;
        destination[static_cast<std::size_t>(y) * screen_width + x] =
            snapshot[static_cast<std::size_t>(source_y) * source_width + static_cast<std::size_t>(source_x)];
    }
}

void compare_frame(const SdramController& memory, unsigned id, const Pixels& expected) {
    for (std::size_t pixel = 0; pixel < expected.size(); pixel += 2) {
        const auto packed = expected[pixel] | (static_cast<std::uint32_t>(expected[pixel + 1]) << 16);
        expect(memory.inspect_word(color_buffer_base(id) + static_cast<std::uint32_t>(pixel * 2)) == packed,
               "copied framebuffer or neighboring masked half-word differs at pixel " + std::to_string(pixel));
    }
}

CopyStatistics execute(SystemCopy& copy, SdramController& memory, OpaqueTile& tile,
                       const CopyCommand& command, bool stalls = false) {
    copy.begin(command);
    for (unsigned guard = 0; copy.busy(); ++guard) {
        expect(guard < 1000000, "clocked system copy hung");
        copy.tick(memory, tile, !stalls || guard % 5 != 1);
        memory.tick();
    }
    expect(!copy.memory_pending() && !memory.response_ready(MemoryClient::Copy),
           "copy retired before destination acknowledgement drained");
    return copy.statistics();
}

void overlap_tests() {
    const std::array<Point, 6> offsets{{{3, 0}, {-3, 0}, {0, 3}, {0, -3}, {3, 3}, {-3, -3}}};
    for (const auto offset : offsets) test("snapshot overlap " + std::to_string(offset.x) + "," + std::to_string(offset.y), [&] {
        SdramController memory; boot(memory);
        auto expected = seed_frame(memory, 1);
        const CopyCommand command{false, 1, 1, {9, 10}, {9 + offset.x, 10 + offset.y}, 51, 41, {}};
        expected_copy(expected, expected, screen_width, screen_height, command);
        SystemCopy copy; OpaqueTile tile;
        const auto counts = execute(copy, memory, tile, command, offset.x < 0);
        expect(counts.words == 51u * 41u, "overlap lost or duplicated pixels");
        compare_frame(memory, 1, expected);
    });
}

void image_tests() {
    test("clipped NPOT image, padded pitch and odd half-word masks", [] {
        SdramController memory; boot(memory);
        const TextureDescriptor image{heap_base, 23, 17, 52};
        const auto source = seed_image(memory, image);
        auto expected = blank();
        SystemCopy copy; OpaqueTile tile;
        CopyCommand command{true, 0, 1, {-2, -3}, {-4, 475}, 30, 25, image};
        expected_copy(expected, source, image.width, image.height, command);
        expect(execute(copy, memory, tile, command).words == 42, "negative-origin clipping extent differs");
        compare_frame(memory, 1, expected);
        command = {true, 0, 1, {1, 2}, {635, 479}, 22, 12, image};
        expected_copy(expected, source, image.width, image.height, command);
        expect(execute(copy, memory, tile, command, true).words == 5, "right/bottom clipping extent differs");
        compare_frame(memory, 1, expected);
    });
    test("heap-tail partial reads stay in allocation and depth is untouched", [] {
        SdramController memory; boot(memory);
        const TextureDescriptor image{memory_bytes - 16, 3, 2, 8};
        const auto source = seed_image(memory, image);
        std::vector<std::uint32_t> depth(16);
        for (unsigned i = 0; i < depth.size(); ++i) depth[i] = 0x00abc000u + i;
        upload(memory, depth_buffer_base, depth);
        SystemCopy copy; OpaqueTile tile;
        const CopyCommand command{true, 0, 1, {1, 1}, {639, 479}, 2, 1, image};
        auto expected = blank(); expected_copy(expected, source, 3, 2, command);
        const auto before = memory.statistics().clients[static_cast<unsigned>(MemoryClient::Copy)].bytes_read;
        expect(execute(copy, memory, tile, command).words == 1, "heap-tail copy extent differs");
        const auto after = memory.statistics().clients[static_cast<unsigned>(MemoryClient::Copy)].bytes_read;
        expect(after - before == 4, "heap-tail request fetched outside the clipped allocation word");
        compare_frame(memory, 1, expected);
        for (unsigned i = 0; i < depth.size(); ++i)
            expect(memory.inspect_word(depth_buffer_base + 4u * i) == depth[i], "RGB565 copy wrote shared depth");
    });
    test("full-width row is buffered before the first destination write", [] {
        SdramController memory; boot(memory);
        auto expected = seed_frame(memory, 1, 640, 3);
        const CopyCommand command{false, 1, 1, {0, 0}, {0, 1}, 640, 2, {}};
        expected_copy(expected, expected, screen_width, screen_height, command);
        SystemCopy copy; OpaqueTile tile; copy.begin(command);
        for (unsigned guard = 0; copy.busy(); ++guard) {
            expect(guard < 100000, "full-width copy hung");
            copy.tick(memory, tile); memory.tick();
            const auto& traffic = memory.statistics().clients[static_cast<unsigned>(MemoryClient::Copy)];
            if (traffic.bytes_written != 0) expect(traffic.bytes_read >= 1280, "destination write preceded complete source row");
        }
        expect(copy.statistics().words == 1280, "640-pixel scratch row lost pixels");
        compare_frame(memory, 1, expected);
    });
}

void control_tests() {
    test("validation and fully clipped extreme coordinates leave memory untouched", [] {
        SdramController memory; boot(memory);
        SystemCopy copy; OpaqueTile tile;
        const auto before = memory.cycles();
        CopyCommand command{false, 0, 1, {0, 0}, {std::numeric_limits<int>::max(), 0}, 10, 10, {}};
        copy.begin(command);
        expect(!copy.busy() && copy.statistics().words == 0 && memory.cycles() == before,
               "fully clipped command touched external state");
        command.source_origin = {std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
        command.destination_origin = {0, 0}; command.width = std::numeric_limits<int>::max();
        command.height = std::numeric_limits<int>::max(); copy.begin(command);
        expect(!copy.busy(), "extreme clipping wrapped into visible samples");
        const auto counts = copy.statistics();
        command.width = -1;
        throws([&] { copy.begin(command); }, "negative copy size accepted");
        expect(copy.statistics().cycles == counts.cycles, "invalid capture mutated counters");
        command = {true, 0, 1, {}, {}, 1, 1, {memory_bytes - 4, 2, 2, 4}};
        throws([&] { copy.begin(command); }, "heap extent overflow accepted");
        command.image = {heap_base, 1, 1, 3};
        throws([&] { copy.begin(command); }, "misaligned image pitch accepted");
        expect(memory.statistics().clients[static_cast<unsigned>(MemoryClient::Copy)].requests == 0,
               "rejected or empty command issued memory requests");
        (void)tile;
    });
    test("busy capture and reset preserve an accepted asynchronous response", [] {
        SdramController memory; boot(memory);
        const TextureDescriptor image{heap_base, 8, 8, 16}; seed_image(memory, image);
        const CopyCommand command{true, 0, 1, {}, {1, 1}, 8, 8, image};
        SystemCopy copy; OpaqueTile tile; copy.begin(command);
        throws([&] { copy.begin(command); }, "active copy was overwritten");
        for (unsigned guard = 0; !copy.memory_pending(); ++guard) {
            expect(guard < 100, "copy failed to issue source read"); copy.tick(memory, tile); memory.tick();
        }
        throws([&] { copy.reset(); }, "reset abandoned an accepted SDRAM transaction");
        for (unsigned guard = 0; !memory.response_ready(MemoryClient::Copy); ++guard) {
            expect(guard < 10000, "source response hung"); copy.tick(memory, tile, false); memory.tick();
        }
        MemoryRequest extra; extra.address = image.base; extra.words = 1;
        expect(!memory.submit(MemoryClient::Copy, extra), "response backpressure allowed slot overwrite");
        copy.tick(memory, tile); memory.tick();
        expect(!copy.memory_pending(), "ready response was not consumed");
        copy.reset(); expect(!copy.busy(), "drained copy could not reset");
        compare_frame(memory, 1, blank());
    });
    test("shared memory timing changes actual copy completion cycles", [] {
        const auto measure = [](SdramConfig config, bool stalled) {
            SdramController memory(config); boot(memory);
            const TextureDescriptor image{heap_base, 64, 4, 128}; const auto source = seed_image(memory, image);
            const CopyCommand command{true, 0, 1, {}, {1, 17}, 63, 4, image};
            SystemCopy copy; OpaqueTile tile;
            const auto counts = execute(copy, memory, tile, command, stalled);
            auto expected = blank(); expected_copy(expected, source, 64, 4, command);
            compare_frame(memory, 1, expected);
            expect(counts.words == 252, "timing variation lost copy data");
            return counts.cycles;
        };
        SdramConfig fast;
        SdramConfig slow; slow.t_rp = 8; slow.t_rcd = 9; slow.t_ras = 12; slow.t_rc = 20;
        slow.t_rfc = 20; slow.refresh_interval_cycles = 500;
        const auto normal = measure(fast, false);
        expect(measure(slow, false) > normal, "SDRAM timing did not delay copy execution");
        expect(measure(fast, true) > normal, "disabled engine edges did not delay completion");
    });
}

} // namespace

int main() {
    overlap_tests(); image_tests(); control_tests();
    std::cout << cases - failures << '/' << cases << " system copy cases passed\n";
    return failures == 0 ? 0 : 1;
}
