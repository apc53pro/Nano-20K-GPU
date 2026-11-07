#include "Vnano_gpu_core.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include "sdram_model.hpp"
#include "nano_gpu/demo.hpp"
#include "nano_gpu/hw/protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef NANO_GPU_RTL_SDL
#include <SDL.h>
#endif

namespace {
using namespace nano_gpu;
struct WindowClosed {};

struct Options {
    bool headless = false, verify = false, self_test = false;
    unsigned frames = 0;
    DemoScene scene = DemoScene::Showcase;
    long double time = 0, step = 1.0L / 60;
    std::string stats, output, trace, scanout_log;
};

Options parse(int argc, char** argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (++i == argc) throw std::invalid_argument("missing value for " + arg);
            return argv[i];
        };
        if (arg == "--headless") out.headless = true;
        else if (arg == "--self-test") { out.self_test = true; out.headless = true; }
        else if (arg == "--verify") out.verify = true;
        else if (arg == "--frames") out.frames = static_cast<unsigned>(std::stoul(value()));
        else if (arg == "--time") out.time = std::stold(value());
        else if (arg == "--step") out.step = std::stold(value());
        else if (arg == "--stats") out.stats = value();
        else if (arg == "--output") out.output = value();
        else if (arg == "--trace") out.trace = value();
        else if (arg == "--scanout-log") out.scanout_log = value();
        else if (arg == "--scene") {
            const auto name = value();
            if (name == "showcase") out.scene = DemoScene::Showcase;
            else if (name == "perspective") out.scene = DemoScene::Perspective;
            else if (name == "overdraw") out.scene = DemoScene::Overdraw;
            else if (name == "opaque") out.scene = DemoScene::Opaque;
            else throw std::invalid_argument("unknown scene " + name);
        } else if (arg == "--help") {
            std::cout << "RTL GPU (Verilator; C++ is host, external SDRAM and independent oracle)\n"
                "--headless --verify --scene showcase|perspective|overdraw|opaque\n"
                "--frames N --time S --step S --stats FILE.json --output FILE.ppm --trace FILE.vcd\n"
                "--scanout-log FILE.csv (every complete displayed frame, including failures)\n"
                "--self-test (command, copy, LOAD and recovery regressions)\n";
            std::exit(0);
        } else throw std::invalid_argument("unknown argument " + arg);
    }
    if (out.headless && out.frames == 0) out.frames = 1;
    if (out.frames > 1000000) throw std::invalid_argument("too many frames");
    return out;
}

std::uint64_t hash(const std::vector<std::uint16_t>& pixels) {
    std::uint64_t result = 14695981039346656037ull;
    for (auto pixel : pixels) for (unsigned byte = 0; byte != 2; ++byte) {
        result ^= static_cast<std::uint8_t>(pixel >> (8 * byte)); result *= 1099511628211ull;
    }
    return result;
}

std::string hash_string(const std::vector<std::uint16_t>& pixels) {
    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(16) << hash(pixels);
    return result.str();
}

class Simulation {
public:
    VerilatedContext context;
    Vnano_gpu_core dut{&context};
    rtl::SdramPinsModel memory;
    std::unique_ptr<VerilatedVcdC> trace;
    std::ofstream scanout_log;
    std::vector<std::uint16_t> working = std::vector<std::uint16_t>(screen_width * screen_height), display = working;
    std::vector<std::uint16_t> expected_scanout;
    std::uint64_t cycles = 0, completed_frames = 0, last_submission_cycles = 0;
    std::uint64_t checked_scanout_frames = 0, black_scanout_frames = 0;
    unsigned active_pixels = 0;
    std::string phase = "boot";
    bool host_rising = false, host_falling = false, write_accepted = false;
    std::function<void()> pump;

    explicit Simulation(const std::string& trace_path, const std::string& scanout_path = {}) {
        dut.clk = 0; dut.reset = 1; dut.host_clk = 0; dut.abort_request = 0;
        dut.host_wr_valid = 0; dut.host_wr_data = 0; dut.host_rd_req = 0; dut.host_reg_addr = 0;
        dut.sd_dq_in = 0xcdcdcdcdu; dut.pixel_advance = 0;
        if (!scanout_path.empty()) {
            scanout_log.open(scanout_path);
            if (!scanout_log) throw std::runtime_error("cannot open scanout log " + scanout_path);
            scanout_log << "frame,core_cycle,phase,nonzero_pixels,color_fnv1a64,verified\n";
        }
        if (!trace_path.empty()) {
            context.traceEverOn(true); trace = std::make_unique<VerilatedVcdC>();
            dut.trace(trace.get(), 8); trace->open(trace_path.c_str());
        }
        for (unsigned edge = 0; edge < 16; ++edge) tick(false);
        dut.reset = 0;
        wait([&] { return dut.ready; }, "SDRAM initialization and attachment boot");
        phase = "idle";
    }
    ~Simulation() { if (trace) trace->close(); dut.final(); }

    void tick(bool check_fault = true) {
        dut.clk = 0; dut.eval(); context.timeInc(1); if (trace) trace->dump(context.time());
#ifdef NANO_GPU_BOARD_PROFILE
        // Board profile: 50.4 MHz core, one external pixel tick per two core
        // cycles (25.2 MHz) and the virtual host at the same rate.
        const bool next_host = (cycles & 1u) != 0;
        dut.pixel_advance = (cycles & 1u) != 0;
#else
        const bool next_host = (cycles & 3u) >= 2u;
#endif
        host_rising = !dut.host_clk && next_host;
        host_falling = dut.host_clk && !next_host;
        write_accepted = host_rising && dut.host_wr_valid && dut.host_wr_ready;
        dut.host_clk = next_host; dut.eval();
        rtl::SdramPinsModel::Pins pins;
        pins.cke=dut.sd_cke; pins.cs_n=dut.sd_cs_n; pins.ras_n=dut.sd_ras_n;
        pins.cas_n=dut.sd_cas_n; pins.we_n=dut.sd_we_n; pins.bank=dut.sd_ba;
        pins.address=dut.sd_addr; pins.dq_oe=dut.sd_dq_oe; pins.dq_out=dut.sd_dq_out; pins.dqm=dut.sd_dqm;
        const bool pixel_tick = dut.video_pixel_ce;
        dut.clk = 1; dut.eval();
        dut.sd_dq_in = memory.rising_edge(pins); dut.eval();
        context.timeInc(1); if (trace) trace->dump(context.time());
        ++cycles;
        if (pixel_tick && !dut.reset) {
            if (dut.video_de && dut.video_x < screen_width && dut.video_y < screen_height) {
                if (dut.video_x == 0 && dut.video_y == 0) active_pixels = 0;
                working[dut.video_y * screen_width + dut.video_x] = dut.video_rgb565;
                ++active_pixels;
                if (dut.video_x == screen_width - 1 && dut.video_y == screen_height - 1) {
                    if (active_pixels != screen_width * screen_height)
                        throw std::runtime_error("incomplete RTL scanout frame");
                    display = working; ++completed_frames;
                    const auto nonzero = std::count_if(display.begin(), display.end(), [](auto pixel) { return pixel != 0; });
                    if (nonzero == 0) ++black_scanout_frames;
                    const bool checked = !expected_scanout.empty();
                    const bool matched = !checked || display == expected_scanout;
                    if (scanout_log) {
                        scanout_log << completed_frames << ',' << cycles << ',' << phase << ',' << nonzero << ','
                                    << hash_string(display) << ',' << (checked && matched ? "true" : "false") << '\n';
                        scanout_log.flush();
                    }
                    if (checked) {
                        ++checked_scanout_frames;
                        if (!matched) throw std::runtime_error("RTL scanout mismatch frame=" + std::to_string(completed_frames) +
                            " phase=" + phase + " cycle=" + std::to_string(cycles) +
                            " expected=" + hash_string(expected_scanout) + " actual=" + hash_string(display));
                    }
                }
            }
        }
        if (check_fault && dut.fault)
            throw std::runtime_error("RTL GPU fault " + std::to_string(dut.fault_code) + " at cycle " + std::to_string(cycles));
        if (pump && cycles % 100000 == 0) pump();
        if (context.gotFinish()) throw std::runtime_error("RTL simulation finished unexpectedly");
    }
    template<class Predicate> void wait(Predicate predicate, const std::string& operation) {
        const auto start = cycles;
        while (!predicate()) {
            tick();
            if (cycles - start > 500000000ull) throw std::runtime_error("RTL timeout: " + operation);
        }
    }
    void send(const std::vector<std::uint8_t>& bytes) {
        dut.host_rd_req = 0; dut.host_reg_addr = 0;
        for (const auto byte : bytes) {
            while (dut.host_clk) tick(); // Change data at a falling host edge.
            dut.host_wr_data = byte; dut.host_wr_valid = 1;
            do { tick(); } while (!write_accepted);
        }
        while (dut.host_clk) tick();
        dut.host_wr_valid = 0; tick();
    }
    std::vector<std::uint8_t> reply(hw::Opcode opcode, std::uint32_t sequence) {
        std::vector<std::uint8_t> result;
        dut.host_wr_valid = 0; dut.host_reg_addr = 0; dut.host_rd_req = 1;
        auto expected = std::size_t{16}; const auto start = cycles;
        while (result.size() < expected) {
            tick();
            if (host_falling && dut.host_rd_valid) {
                result.push_back(dut.host_rd_data);
                if (result.size() == 16) {
                    if (hw::read_u32(result, 0) != 0x5550474e || result[4] != 0 || result[5] != 2 ||
                        hw::read_u16(result, 6) != (static_cast<unsigned>(opcode) | 0x8000u) ||
                        hw::read_u32(result, 12) != sequence || hw::read_u32(result, 8) > 780)
                        throw std::runtime_error("invalid RTL response header");
                    expected += hw::read_u32(result, 8);
                }
            }
            if (cycles - start > 500000000ull) throw std::runtime_error("RTL response timed out");
        }
        dut.host_rd_req = 0;
        if (result.size() < 28 || hw::read_u32(result, 20) != 0) throw std::runtime_error("RTL command failed");
        return result;
    }
    void render(const hw::HostProgram& program) {
        phase = "submit";
        const auto start = cycles; send(program.bytes);
        const auto response = reply(hw::Opcode::Fence, program.fence_sequence);
        if (hw::read_u32(response, 24) != program.fence_token) throw std::runtime_error("wrong RTL fence token");
        last_submission_cycles = cycles - start;
        phase = "idle";
    }
    std::vector<std::uint8_t> command(hw::Opcode opcode, std::uint32_t sequence,
                                      const std::vector<std::uint8_t>& payload = {}) {
        send(hw::encode_packet(opcode, sequence, payload));
        return reply(opcode, sequence);
    }
    std::uint8_t read_register(unsigned address) {
        while (dut.host_clk) tick();
        dut.host_wr_valid = 0; dut.host_reg_addr = address; dut.host_rd_req = 1;
        const auto start = cycles;
        do {
            tick();
            if (cycles - start > 1000000) throw std::runtime_error("host register read timed out");
        } while (!host_falling || !dut.host_rd_valid);
        const auto value = dut.host_rd_data;
        dut.host_rd_req = 0; dut.host_reg_addr = 0;
        return value;
    }
    std::uint32_t scanout_underflows() {
        (void)read_register(1); // Coherent sideband snapshot; no command queue traffic.
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte)
            value |= static_cast<std::uint32_t>(read_register(24 + byte)) << (byte * 8);
        return value;
    }
    void recover() {
        // The external chip is deliberately not reset or replaced.
        dut.host_wr_valid = 0; dut.host_rd_req = 0; dut.abort_request = 1;
        for (unsigned edge = 0; edge < 32; ++edge) tick(false);
        dut.abort_request = 0;
        const auto start = cycles;
        do {
            tick(false);
            if (cycles - start > 1000000) throw std::runtime_error("abort recovery timed out");
        } while (!dut.ready || dut.busy || dut.fault);
    }
    void present(int target, std::uint32_t sequence, const std::vector<std::uint16_t>* expected = nullptr) {
        phase = "present";
        std::vector<std::uint8_t> payload(8);
        hw::write_u32(payload, 0, target); hw::write_u32(payload, 4, sequence);
        send(hw::encode_packet(hw::Opcode::Present, sequence, payload));
        (void)reply(hw::Opcode::Present, sequence);
        // PRESENT replies during vertical blank. Publish at the final active
        // pixel, so the next completed capture must belong to the new front.
        // Keep checking that front during every later upload/render/idle frame.
        if (expected) expected_scanout = *expected;
        phase = "observe";
        const auto before = completed_frames;
        wait([&] { return completed_frames >= before + 1; }, "complete actual scanout");
        phase = "idle";
    }
    void compare(const Renderer& oracle) const {
        for (unsigned target = 0; target < 2; ++target) for (unsigned index = 0; index < screen_width * screen_height; ++index) {
            const auto packed = memory.inspect_word(hw::color_buffer_base(target) + (index & ~1u) * 2);
            const auto value = static_cast<std::uint16_t>(packed >> (16 * (index & 1u)));
            if (value != oracle.color(target)[index]) throw std::runtime_error("RTL RGB565 mismatch buffer=" +
                std::to_string(target) + " x=" + std::to_string(index % screen_width) + " y=" + std::to_string(index / screen_width) +
                " expected=" + std::to_string(oracle.color(target)[index]) + " actual=" + std::to_string(value));
        }
        for (unsigned index = 0; index < screen_width * screen_height; ++index) {
            const auto value = memory.inspect_word(hw::depth_buffer_base + index * 4);
            if (value != oracle.depth()[index]) throw std::runtime_error("RTL D24 mismatch at " + std::to_string(index) +
                " expected=" + std::to_string(oracle.depth()[index]) + " actual=" + std::to_string(value));
        }
    }
};

void self_test(Simulation& simulation) {
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(std::string("RTL self-test: ") + message);
    };
    std::uint32_t sequence = 1;
    const auto caps = simulation.command(hw::Opcode::GetCaps, sequence++);
    require(caps.size() == 92 && hw::read_u32(caps, 32) == 8 * 1024 * 1024 &&
            hw::read_u32(caps, 80) == 640 && hw::read_u32(caps, 84) == 480, "capability reply");
    const auto status = simulation.command(hw::Opcode::GetStatus, sequence++);
    require(status.size() == 92 && hw::read_u32(status, 30) == 1, "initial coherent status epoch");

    const auto upload = [&](std::uint32_t address, const std::vector<std::uint8_t>& bytes) {
        for (std::size_t cursor = 0; cursor < bytes.size(); cursor += hw::maximum_upload_decoded_bytes) {
            const auto count = std::min(hw::maximum_upload_decoded_bytes, bytes.size() - cursor);
            const std::vector<std::uint8_t> chunk(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                bytes.begin() + static_cast<std::ptrdiff_t>(cursor + count));
            simulation.send(hw::encode_packet(hw::Opcode::WriteHeap, sequence++,
                hw::encode_heap_upload(address + static_cast<std::uint32_t>(cursor), chunk)));
        }
    };
    const auto readback = [&](std::uint32_t address, unsigned length) {
        std::vector<std::uint8_t> payload(12);
        hw::write_u32(payload, 0, address); hw::write_u32(payload, 4, length);
        hw::write_u32(payload, 8, 0x12345678);
        const auto result = simulation.command(hw::Opcode::ReadMemory, sequence++, payload);
        require(result.size() == 36 + length && hw::read_u32(result, 24) == 0x12345678 &&
                hw::read_u32(result, 28) == address && hw::read_u32(result, 32) == length, "readback metadata");
        return std::vector<std::uint8_t>(result.begin() + 36, result.end());
    };
    const std::uint32_t heap = hw::heap_base;
    std::vector<std::uint8_t> bytes(513);
    for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i * 37 + 11);
    upload(heap, bytes);
    require(readback(heap + 1, 512) == std::vector<std::uint8_t>(bytes.begin() + 1, bytes.end()),
            "unaligned multi-burst heap readback");
    upload(heap + 3, {0xa1, 0xb2, 0xc3});
    bytes[3] = 0xa1; bytes[4] = 0xb2; bytes[5] = 0xc3;
    require(readback(heap, 16) == std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 16),
            "byte masks preserve neighboring heap bytes");
    // Cross every signed-width boundary and the word/group/chunk boundaries
    // with all four upload codes. The final unaligned byte must not expose
    // the codec's deterministic zero-padding beyond the decoded extent.
    const std::array<std::uint32_t, 12> upload_words{{0, 0x7f, 0xffffff80u, 0x80,
        0xffffff7fu, 0x7fff, 0xffff8000u, 0x8000, 0xffff7fffu,
        0x80000000u, 0x7fffffffu, 0x89abcdefu}};
    std::vector<std::uint8_t> codec_bytes(hw::maximum_upload_decoded_bytes + 17, 0);
    for (std::size_t word = 0; 4 * word + 4 <= codec_bytes.size(); ++word)
        hw::write_u32(codec_bytes, 4 * word, upload_words[word % upload_words.size()]);
    codec_bytes.back() = 0xa7;
    const auto codec_address = heap + 1027;
    upload(codec_address - 1, std::vector<std::uint8_t>(codec_bytes.size() + 2, 0x5a));
    upload(codec_address, codec_bytes);
    for (std::size_t cursor = 0; cursor < codec_bytes.size(); cursor += 512) {
        const auto count = std::min<std::size_t>(512, codec_bytes.size() - cursor);
        require(readback(codec_address + static_cast<std::uint32_t>(cursor), count) ==
            std::vector<std::uint8_t>(codec_bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                     codec_bytes.begin() + static_cast<std::ptrdiff_t>(cursor + count)),
            "encoded upload preserves signed widths and chunk/group boundaries");
    }
    require(readback(codec_address - 1, 1) == std::vector<std::uint8_t>({0x5a}) &&
            readback(codec_address + codec_bytes.size(), 1) == std::vector<std::uint8_t>({0x5a}),
            "encoded tail padding preserves neighboring byte guards");

    Renderer oracle;
    Pass pass; pass.clear_color = 0x18e3;
    pass.primitives.push_back(Rect{{3, 5, 31, 25}, overlay_state({241, 63, 27}), 0});
    // Give clipped/overlapping copies distinguishable source pixels, including
    // the far edges and the first two columns used by extreme-origin cases.
    pass.primitives.push_back(Rect{{0, 0, 1, 480}, overlay_state({17, 221, 83}), 0});
    pass.primitives.push_back(Rect{{1, 0, 2, 480}, overlay_state({73, 29, 243}), 0});
    pass.primitives.push_back(Rect{{637, 0, 640, 480}, overlay_state({231, 173, 19}), 0});
    pass.primitives.push_back(Rect{{0, 477, 640, 480}, overlay_state({29, 137, 241}), 0});
    auto program = hw::compile_pass(pass, sequence); sequence = program.next_sequence;
    simulation.render(program); oracle.render(pass, RenderMode::Monolithic); simulation.compare(oracle);
    hw::CopyCommand copy; copy.source = 1; copy.target = 1;
    copy.source_origin = {3, 5}; copy.destination_origin = {9, 8}; copy.width = 28; copy.height = 20;
    simulation.send(hw::encode_packet(hw::Opcode::CopyRect, sequence++, hw::encode_copy_command(copy)));
    std::vector<std::uint8_t> token(4); hw::write_u32(token, 0, 0xf00d);
    (void)simulation.command(hw::Opcode::Fence, sequence++, token);
    oracle.copy_rect(1, 1, copy.source_origin, copy.destination_origin, copy.width, copy.height, RenderMode::Monolithic);
    simulation.compare(oracle);
    // Generic pitched heap images, clipped destination and odd 16-bit lane.
    Image image(7, 5, 20); std::vector<std::uint8_t> image_bytes(100, 0);
    for (int y = 0; y < 5; ++y) for (int x = 0; x < 7; ++x) {
        image.at(x, y) = static_cast<std::uint16_t>(0x3100 + x * 13 + y * 71);
        hw::write_u16(image_bytes, y * 20 + x * 2, image.at(x, y));
    }
    upload(heap, image_bytes); copy.image_source = true; copy.source = 0;
    copy.image = {heap, 7, 5, 20}; copy.source_origin = {0, 0}; copy.destination_origin = {-2, 29}; copy.width = 7; copy.height = 5;
    simulation.send(hw::encode_packet(hw::Opcode::CopyRect, sequence++, hw::encode_copy_command(copy)));
    (void)simulation.command(hw::Opcode::Fence, sequence++, token);
    oracle.copy_image(image, 1, copy.source_origin, copy.destination_origin, copy.width, copy.height, RenderMode::Monolithic);
    simulation.compare(oracle);

    const auto check_copy = [&](const char* name, const hw::CopyCommand& command) {
        try {
            simulation.send(hw::encode_packet(hw::Opcode::CopyRect, sequence++, hw::encode_copy_command(command)));
            (void)simulation.command(hw::Opcode::Fence, sequence++, token);
            if (command.image_source)
                oracle.copy_image(image, command.target, command.source_origin, command.destination_origin,
                                  command.width, command.height, RenderMode::Monolithic);
            else
                oracle.copy_rect(command.source, command.target, command.source_origin, command.destination_origin,
                                 command.width, command.height, RenderMode::Monolithic);
            // Compare both complete color buffers and every depth word, so a
            // clipped/no-op copy must also preserve all untouched attachments.
            simulation.compare(oracle);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("RTL COPY case ") + name + ": " + error.what());
        }
    };
    struct CopyCase {
        const char* name;
        Point source, destination;
        int width, height;
    };
    const auto int_min = std::numeric_limits<std::int32_t>::min();
    const auto int_max = std::numeric_limits<std::int32_t>::max();
    const std::vector<CopyCase> framebuffer_copies{
        {"horizontal right overlap", {3, 5}, {9, 5}, 28, 20},
        {"horizontal left overlap", {9, 5}, {3, 5}, 28, 20},
        {"upward overlap", {9, 8}, {3, 2}, 28, 20},
        {"negative source X", {-2, 4}, {21, 31}, 8, 6},
        {"negative source Y", {4, -2}, {31, 21}, 8, 6},
        {"source right edge", {637, 6}, {39, 41}, 7, 3},
        {"source bottom edge", {5, 477}, {50, 45}, 3, 7},
        {"negative destination X", {3, 5}, {-2, 9}, 8, 4},
        {"negative destination Y", {3, 5}, {9, -2}, 8, 4},
        {"destination right/bottom edge", {3, 5}, {637, 477}, 10, 9},
        {"INT32_MIN source X", {int_min, 0}, {0, 0}, int_max, 1},
        {"INT32_MIN source Y", {0, int_min}, {0, 0}, 1, int_max},
        {"INT32_MAX source X", {int_max, 0}, {0, 0}, int_max, 1},
        {"INT32_MAX source Y", {0, int_max}, {0, 0}, 1, int_max},
        {"INT32_MIN destination X", {0, 0}, {int_min, 0}, int_max, 1},
        {"INT32_MIN destination Y", {0, 0}, {0, int_min}, 1, int_max},
        {"INT32_MAX destination X", {0, 0}, {int_max, 0}, int_max, 1},
        {"INT32_MAX destination Y", {0, 0}, {0, int_max}, 1, int_max},
        {"extreme X offsets with one visible pixel", {-int_max + 2, 5}, {-int_max + 1, 5}, int_max, 2},
        {"extreme Y offsets with visible pixels", {3, -int_max + 3}, {10, -int_max + 2}, 2, int_max},
        {"INT32_MAX extents clipped to two pixels", {637, 477}, {638, 478}, int_max, int_max},
        {"zero width with extreme origins", {int_min, int_max}, {int_max, int_min}, 0, int_max},
        {"zero height with extreme origins", {int_max, int_min}, {int_min, int_max}, int_max, 0},
        // Burst row copy: every source/destination pixel phase, odd and even
        // widths, rows spanning several 16-word bursts and one-pixel shifts.
        {"phase 0 to 1 odd width multi-burst", {4, 10}, {41, 70}, 101, 3},
        {"phase 1 to 0 odd width multi-burst", {5, 10}, {40, 74}, 101, 3},
        {"phase 1 to 1 even width", {7, 12}, {33, 80}, 100, 2},
        {"phase 0 to 0 odd width", {8, 12}, {32, 84}, 99, 2},
        {"phase 0 to 1 even width", {8, 13}, {33, 88}, 100, 2},
        {"phase 1 to 0 even width", {9, 13}, {32, 92}, 100, 2},
        {"full 640-pixel rows", {0, 100}, {0, 200}, 640, 4},
        {"same-row right shift by one", {10, 300}, {11, 300}, 300, 2},
        {"same-row left shift by one", {11, 302}, {10, 302}, 300, 2},
        {"downward overlap odd phase", {3, 310}, {6, 315}, 255, 12},
    };
    copy = {}; copy.source = 1; copy.target = 1;
    for (const auto& item : framebuffer_copies) {
        copy.source_origin = item.source; copy.destination_origin = item.destination;
        copy.width = item.width; copy.height = item.height;
        check_copy(item.name, copy);
    }
    // Reading the displayed front is legal; only the back may be written.
    copy.source = 0; copy.source_origin = {0, 0}; copy.destination_origin = {101, 103};
    copy.width = 5; copy.height = 3; check_copy("front-to-back source", copy);
    // The terminal's two large copies: whole front to back, and a front copy
    // shifted up one 16-pixel text row.
    copy.source_origin = {0, 0}; copy.destination_origin = {0, 0};
    copy.width = 640; copy.height = 480; check_copy("full-screen front-to-back", copy);
    copy.source_origin = {0, 16}; copy.destination_origin = {0, 0};
    copy.width = 640; copy.height = 464; check_copy("one-row scroll front-to-back", copy);
    const std::vector<CopyCase> image_copies{
        {"pitched image clipped source", {-2, -1}, {40, 60}, 10, 9},
        {"pitched image clipped destination", {0, 0}, {-3, -2}, 7, 5},
        {"pitched image all upper edges", {5, 3}, {639, 479}, 7, 5},
        {"pitched image huge extents", {6, 4}, {60, 70}, int_max, int_max},
        {"pitched image extreme visible X", {-int_max + 2, 2}, {-int_max + 1, 7}, int_max, 2},
        {"pitched image odd source to odd destination", {1, 0}, {41, 61}, 6, 5},
        {"pitched image odd source to even destination", {1, 1}, {42, 62}, 6, 4},
        {"pitched image even source to odd destination", {0, 1}, {43, 63}, 5, 4},
    };
    copy.image_source = true; copy.source = 0; copy.image = {heap, 7, 5, 20};
    for (const auto& item : image_copies) {
        copy.source_origin = item.source; copy.destination_origin = item.destination;
        copy.width = item.width; copy.height = item.height;
        check_copy(item.name, copy);
    }
    std::cout << framebuffer_copies.size() + image_copies.size() + 3
              << " additional COPY clipping/extreme/overlap/phase/burst oracle cases passed\n";

    pass.color_load = LoadOp::Load; pass.depth_load = LoadOp::Load; pass.primitives.clear();
    pass.primitives.push_back(Line{{0, 0}, {639, 479}, overlay_state({17, 191, 97}), 0});
    program = hw::compile_pass(pass, sequence); sequence = program.next_sequence;
    simulation.render(program); oracle.render(pass, RenderMode::Monolithic); simulation.compare(oracle);
    simulation.present(1, sequence++, &oracle.color(1)); oracle.queue_present(1); oracle.vertical_blank();
    require(simulation.display == oracle.front_color(), "post-copy/LOAD scanout");
    const auto before_upload_status = simulation.command(hw::Opcode::GetStatus, sequence++);
    const auto before_underflows = hw::read_u32(before_upload_status, 52);
    simulation.phase = "upload-stress";
    for (unsigned prefix = 0; prefix < 4; ++prefix) for (unsigned length : {1u, 2u, 3u, 4u, 60u, 61u, 62u, 63u, 64u, 65u, 70u}) {
        std::vector<std::uint8_t> expected(128, 0x5a), patch(length);
        for (unsigned i = 0; i < length; ++i) patch[i] = static_cast<std::uint8_t>(prefix * 47 + i * 13 + length);
        upload(heap, expected); upload(heap + prefix, patch);
        std::copy(patch.begin(), patch.end(), expected.begin() + prefix);
        require(readback(heap, 128) == expected, "staged upload prefix/tail byte masks");
    }
    std::vector<std::uint8_t> burst(hw::maximum_upload_decoded_bytes);
    for (unsigned i = 0; i < burst.size(); ++i) burst[i] = static_cast<std::uint8_t>(i * 29 + 7);
    // Maximum upload size, an unaligned three-byte prefix and the final SDRAM
    // byte all meet in one case. Guard bytes must survive native burst masks.
    const std::uint32_t end_upload = hw::memory_bytes - burst.size() - 1;
    upload(end_upload - 3, {0xa1, 0xb2, 0xc3});
    upload(hw::memory_bytes - 1, {0xe7});
    upload(end_upload, burst);
    require(readback(end_upload, 512) == std::vector<std::uint8_t>(burst.begin(), burst.begin() + 512),
            "maximum unaligned heap upload first 512 bytes");
    require(readback(end_upload + burst.size() - 512, 512) == std::vector<std::uint8_t>(burst.end() - 512, burst.end()),
            "maximum unaligned READ_MEMORY at heap end");
    require(readback(end_upload - 3, 3) == std::vector<std::uint8_t>({0xa1, 0xb2, 0xc3}) &&
            readback(hw::memory_bytes - 1, 1) == std::vector<std::uint8_t>({0xe7}),
            "maximum upload preserves prefix and final SDRAM byte guards");
    const auto before_stress_frames = simulation.checked_scanout_frames;
    for (unsigned i = 0; i < 520; ++i) upload(heap + i * burst.size(), burst);
    (void)simulation.command(hw::Opcode::Fence, sequence++, token);
    const auto after_upload_status = simulation.command(hw::Opcode::GetStatus, sequence++);
    require(hw::read_u32(after_upload_status, 52) == before_underflows, "host uploads cannot starve scanout");
    require(simulation.checked_scanout_frames > before_stress_frames, "upload stress spans a complete displayed frame");
    simulation.phase = "idle";
    const auto before = simulation.memory.inspect_word(hw::color_buffer_base(1) + 6 * screen_width * 2);
    // Deliver a partial packet and cancel it without a memory backdoor.
    const auto partial = hw::encode_packet(hw::Opcode::GetStatus, sequence++);
    simulation.send(std::vector<std::uint8_t>(partial.begin(), partial.begin() + 9));
    simulation.recover(); sequence = 1;
    const auto recovered = simulation.command(hw::Opcode::GetStatus, sequence++);
    require(hw::read_u32(recovered, 16) == 2 && hw::read_u32(recovered, 30) == 2,
            "abort increments epoch and restarts host sequencing");
    require(simulation.memory.inspect_word(hw::color_buffer_base(1) + 6 * screen_width * 2) == before,
            "abort preserves external front buffer");
    // A malformed header must fault in RTL, then recover through the same pins.
    auto malformed = hw::encode_packet(hw::Opcode::GetCaps, sequence++); malformed[0] ^= 1;
    bool observed_fault = false;
    try { simulation.send(malformed); for (unsigned i = 0; i < 10000; ++i) simulation.tick(); }
    catch (const std::runtime_error&) { if (!simulation.dut.fault) throw; observed_fault = true; }
    require(observed_fault, "malformed packet faults");
    simulation.recover();
    require(hw::read_u32(simulation.command(hw::Opcode::GetCaps, 1), 16) == 3, "fault recovery epoch");
    pass.target = 0; pass.color_load = LoadOp::Clear; pass.depth_load = LoadOp::Clear;
    pass.clear_color = 0x7bef; pass.primitives.clear();
    program = hw::compile_pass(pass, 2);
    simulation.send(program.bytes);
    const auto active_wait_start = simulation.cycles;
    for (;;) {
        (void)simulation.read_register(1); // Refresh a coherent out-of-band snapshot.
        if ((simulation.read_register(15) & 1) == 0) break;
        require(simulation.cycles - active_wait_start < 1000000, "pass starts before abort");
    }
    require(simulation.dut.busy, "active pass is in flight before abort");
    simulation.recover();
    const auto cancelled = simulation.command(hw::Opcode::GetStatus, 1);
    if (hw::read_u32(cancelled, 16) != 4 || cancelled.at(43) != 2)
        throw std::runtime_error("RTL self-test: active abort epoch=" + std::to_string(hw::read_u32(cancelled, 16)) +
                                 " validity=" + std::to_string(cancelled.at(43)));
    observed_fault = false;
    try { simulation.present(0, 2); }
    catch (const std::runtime_error&) { if (!simulation.dut.fault) throw; observed_fault = true; }
    require(observed_fault, "invalidated target cannot be presented");
    simulation.recover();
    const auto before_write_abort = simulation.command(hw::Opcode::GetStatus, 1);
    sequence = 2;
    const auto physical_writes = simulation.memory.statistics().write_commands;
    upload(heap, burst);
    simulation.wait([&] { return simulation.memory.statistics().write_commands > physical_writes; },
                    "observe an accepted upload on SDRAM pins before abort");
    simulation.recover();
    require(hw::read_u32(simulation.command(hw::Opcode::GetCaps, 1), 16) == hw::read_u32(before_write_abort, 16) + 1,
            "accepted upload drains and restarts its epoch after abort");
    for (unsigned index = 0; index < screen_width * screen_height; ++index) {
        const auto word = simulation.memory.inspect_word(hw::color_buffer_base(1) + (index & ~1u) * 2);
        require(static_cast<std::uint16_t>(word >> (16 * (index & 1u))) == oracle.front_color()[index],
                "active abort preserves every front-buffer pixel");
    }
    // Bypass the producer's own validation to prove negative extents are
    // rejected by RTL before any SDRAM write, and can recover over host pins.
    for (unsigned extent_offset : {28u, 32u}) {
        hw::CopyCommand invalid_copy; invalid_copy.source = 1; invalid_copy.target = 0;
        invalid_copy.width = 1; invalid_copy.height = 1;
        auto payload = hw::encode_copy_command(invalid_copy);
        hw::write_u32(payload, extent_offset, 0xffffffffu);
        const auto before_bad_copy_writes = simulation.memory.statistics().write_commands;
        observed_fault = false;
        try {
            simulation.send(hw::encode_packet(hw::Opcode::CopyRect, 2, payload));
            for (unsigned edge = 0; edge < 10000; ++edge) simulation.tick();
        } catch (const std::runtime_error&) {
            if (!simulation.dut.fault) throw;
            observed_fault = true;
        }
        require(observed_fault && simulation.dut.fault_code == 4, "negative COPY extent faults in RTL");
        require(simulation.memory.statistics().write_commands == before_bad_copy_writes,
                "negative COPY extent cannot write SDRAM");
        simulation.recover();
    }
    // Submit raw descriptor mutations so the CPU encoder cannot hide a
    // streaming validator defect. Failed descriptors must never start a tile
    // store, even when an invalid field would become legal after truncation.
    struct DescriptorFaultCase {
        const char* name;
        unsigned offset;
        std::uint32_t value, fault;
        bool invalid_load = false;
    };
    const std::vector<DescriptorFaultCase> descriptor_faults{
        {"magic", 0, 0x3153504f, 6},
        {"magic before LOAD", 0, 0x3153504f, 6, true},
        {"target high bit", 4, 2, 6},
        {"target high bit before LOAD", 4, 2, 6, true},
        {"front target", 4, 1, 6},
        {"load flag reserved bit", 8, 4, 6},
        {"RGB565 upper bit", 12, 0x10000, 6},
        {"D24 upper bit", 16, 0x1000000, 6},
        {"unaligned table", 20, heap + 65, 6},
        {"table below heap", 20, heap - 4, 6},
        {"table end outside SDRAM", 20, hw::memory_bytes - 2396, 6},
        {"table high address bit", 20, hw::memory_bytes + heap, 6},
        {"tile count", 24, 299, 6},
        {"reserved word", 28, 1, 6},
        {"invalid color/depth LOAD", 8, 0, 5},
        {"invalid depth LOAD", 8, 1, 5},
        {"invalid color LOAD", 8, 2, 5},
    };
    for (const auto& item : descriptor_faults) {
        sequence = 1; // Each preceding soft recovery starts a fresh stream.
        std::vector<std::uint8_t> descriptor(32, 0), payload(4);
        hw::write_u32(descriptor, 0, 0x3153504e);
        hw::write_u32(descriptor, 8, item.invalid_load ? 0 : 3); // target 0; normal case clears both
        hw::write_u32(descriptor, 16, 0xffffff);
        hw::write_u32(descriptor, 20, heap + 64);
        hw::write_u32(descriptor, 24, 300);
        hw::write_u32(descriptor, item.offset, item.value);
        upload(heap, descriptor);
        (void)simulation.command(hw::Opcode::Fence, sequence++, token);
        hw::write_u32(payload, 0, heap);
        const auto before_bad_descriptor_writes = simulation.memory.statistics().write_commands;
        observed_fault = false;
        try {
            simulation.send(hw::encode_packet(hw::Opcode::SubmitPass, sequence++, payload));
            for (unsigned edge = 0; edge < 10000; ++edge) simulation.tick();
        } catch (const std::runtime_error&) {
            if (!simulation.dut.fault) throw;
            observed_fault = true;
        }
        if (!observed_fault || simulation.dut.fault_code != item.fault)
            throw std::runtime_error(std::string("RTL descriptor fault case ") + item.name +
                                     " expected category " + std::to_string(item.fault));
        require(simulation.memory.statistics().write_commands == before_bad_descriptor_writes,
                "invalid descriptor cannot write SDRAM attachments");
        simulation.recover();
    }
    std::cout << descriptor_faults.size() << " raw descriptor/LOAD fault cases passed\n";
    // These packets deliberately bypass the producer's decoder validation.
    // An accepted prefix of a bad upload may touch heap bytes, but the command
    // must fault rather than consume outside its payload, hang, or retire.
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> upload_faults;
    auto bad_upload = hw::encode_heap_upload(heap, {0x78, 0x56, 0x34, 0x12});
    bad_upload.pop_back(); upload_faults.emplace_back("truncated literal", bad_upload);
    bad_upload = hw::encode_heap_upload(heap, std::vector<std::uint8_t>(20, 0));
    bad_upload.pop_back(); upload_faults.emplace_back("missing next group descriptor", bad_upload);
    bad_upload = hw::encode_heap_upload(heap, {0}); bad_upload.push_back(0);
    upload_faults.emplace_back("trailing bytes", bad_upload);
    bad_upload = hw::encode_heap_upload(heap, {0}); bad_upload[8] |= 4;
    upload_faults.emplace_back("nonzero unused group code", bad_upload);
    for (const auto length : {0u, static_cast<unsigned>(hw::maximum_upload_decoded_bytes + 1)}) {
        bad_upload = hw::encode_heap_upload(heap, {0}); hw::write_u32(bad_upload, 4, length);
        upload_faults.emplace_back("invalid decoded count", bad_upload);
    }
    bad_upload = hw::encode_heap_upload(heap, {1, 2, 3, 4});
    hw::write_u32(bad_upload, 0, hw::memory_bytes - 1);
    upload_faults.emplace_back("decoded range outside heap", bad_upload);
    for (const auto& item : upload_faults) {
        observed_fault = false;
        try {
            simulation.send(hw::encode_packet(hw::Opcode::WriteHeap, 1, item.second));
            for (unsigned edge = 0; edge < 10000; ++edge) simulation.tick();
        } catch (const std::runtime_error&) {
            if (!simulation.dut.fault) throw;
            observed_fault = true;
        }
        if (!observed_fault || simulation.dut.fault_code != 3)
            throw std::runtime_error("RTL encoded upload fault case " + item.first + " did not fault in category3");
        simulation.recover();
    }
    // No compatibility decoder remains: the old envelope must be rejected.
    auto obsolete = hw::encode_packet(hw::Opcode::GetCaps, 1); obsolete[5] = 1;
    observed_fault = false;
    try {
        simulation.send(obsolete);
        for (unsigned edge = 0; edge < 10000; ++edge) simulation.tick();
    } catch (const std::runtime_error&) {
        if (!simulation.dut.fault) throw;
        observed_fault = true;
    }
    require(observed_fault, "obsolete0.1 envelope is rejected");
    simulation.recover();
    require(simulation.command(hw::Opcode::GetCaps, 1).at(5) == 2,
            "codec and obsolete-version faults recover to sole0.2 stream");
    std::cout << upload_faults.size() << " malformed encoded-upload cases and obsolete-version rejection passed\n";
    std::cout << "RTL command/copy/LOAD/scanout/recovery self-test passed\n";
}

#ifdef NANO_GPU_RTL_SDL
class Presenter {
public:
    Presenter() {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow("Nano 20K | RTL / Verilator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  1280, 960, SDL_WINDOW_RESIZABLE);
        if (!window) throw std::runtime_error(SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer || SDL_RenderSetLogicalSize(renderer, screen_width, screen_height)) throw std::runtime_error(SDL_GetError());
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, screen_width, screen_height);
        if (!texture) throw std::runtime_error(SDL_GetError());
    }
    ~Presenter() { SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); }
    bool events() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT ||
            (event.type == SDL_KEYDOWN && (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q))) return false;
        return true;
    }
    void show(const std::vector<std::uint16_t>& pixels) {
        if (SDL_UpdateTexture(texture, nullptr, pixels.data(), screen_width * 2) || SDL_RenderClear(renderer) ||
            SDL_RenderCopy(renderer, texture, nullptr, nullptr)) throw std::runtime_error(SDL_GetError());
        SDL_RenderPresent(renderer);
    }
private:
    SDL_Window* window = nullptr; SDL_Renderer* renderer = nullptr; SDL_Texture* texture = nullptr;
};
#endif

void ppm(const std::string& path, const std::vector<std::uint16_t>& image) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot open output " + path);
    out << "P6\n640 480\n255\n";
    for (auto pixel : image) {
        const auto color = expand_rgb565(pixel);
        out.put(static_cast<char>(color.r)); out.put(static_cast<char>(color.g)); out.put(static_cast<char>(color.b));
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
#ifdef NANO_GPU_RTL_SDL
        std::unique_ptr<Presenter> presenter;
        if (!options.headless) presenter = std::make_unique<Presenter>();
#else
        if (!options.headless) throw std::runtime_error("built without SDL2; use --headless or rebuild with SDL2 development files");
#endif
        Simulation simulation(options.trace, options.scanout_log); Renderer oracle;
        if (options.self_test) { self_test(simulation); return 0; }
#ifdef NANO_GPU_RTL_SDL
        if (presenter) simulation.pump = [&] {
            if (!presenter->events()) throw WindowClosed{};
            presenter->show(simulation.display);
        };
#endif
        std::uint32_t sequence = 1; int target = 1; unsigned frames = 0;
        auto time = options.time;
        const auto start = std::chrono::steady_clock::now();
        while (options.frames == 0 || frames < options.frames) {
#ifdef NANO_GPU_RTL_SDL
            if (presenter && !presenter->events()) break;
#endif
            const auto pass = make_demo_pass(options.scene, target, time);
            const auto program = hw::compile_pass(pass, sequence);
            sequence = program.next_sequence;
            simulation.render(program);
            if (options.verify) { oracle.render(pass, RenderMode::Monolithic); simulation.compare(oracle); }
            simulation.present(target, sequence++, options.verify ? &oracle.color(target) : nullptr);
            if (options.verify) {
                oracle.queue_present(target); oracle.vertical_blank();
                if (simulation.display != oracle.front_color()) throw std::runtime_error("actual RTL scanout differs from the SRR oracle");
            }
#ifdef NANO_GPU_RTL_SDL
            if (presenter) presenter->show(simulation.display);
#endif
            ++frames; target = 1 - target; time += options.step;
        }
        const auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto underflows = simulation.scanout_underflows();
        std::string report = "{\n  \"profile\": \"N20K-RTL-SIM-0.1\",\n  \"backend\": \"verilator\",\n"
            "  \"cpp_device_backends_linked\": false,\n  \"whole_gpu_fit_qualified\": false,\n  \"frames\": " + std::to_string(frames) +
            ",\n  \"verified\": " + (options.verify ? "true" : "false") + ",\n  \"core_cycles\": " + std::to_string(simulation.cycles) +
            ",\n  \"last_submission_cycles\": " + std::to_string(simulation.last_submission_cycles) +
            ",\n  \"checked_scanout_frames\": " + std::to_string(simulation.checked_scanout_frames) +
            ",\n  \"black_scanout_frames_including_startup\": " + std::to_string(simulation.black_scanout_frames) +
            ",\n  \"scanout_underflows\": " + std::to_string(underflows) +
            ",\n  \"wall_seconds_excluding_boot\": " + std::to_string(wall) +
            ",\n  \"color_fnv1a64\": \"" + hash_string(simulation.display) + "\",\n  \"sdram_refreshes\": " +
            std::to_string(simulation.memory.statistics().refreshes) + "\n}\n";
        std::cout << report;
        if (!options.stats.empty()) { std::ofstream out(options.stats); if (!out) throw std::runtime_error("cannot write statistics"); out << report; }
        if (!options.output.empty()) ppm(options.output, simulation.display);
        return 0;
    } catch (const WindowClosed&) { return 0; }
    catch (const std::exception& error) { std::cerr << "nano-gpu-rtl: " << error.what() << '\n'; return 1; }
}
