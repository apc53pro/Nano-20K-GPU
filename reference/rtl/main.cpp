#include "Vnano_gpu_core.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include "sdram_model.hpp"
#include "nano_gpu/demo.hpp"
#include "nano_gpu/hw/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <iomanip>
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
    std::string stats, output, trace;
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

class Simulation {
public:
    VerilatedContext context;
    Vnano_gpu_core dut{&context};
    rtl::SdramPinsModel memory;
    std::unique_ptr<VerilatedVcdC> trace;
    std::vector<std::uint16_t> working = std::vector<std::uint16_t>(screen_width * screen_height), display = working;
    std::uint64_t cycles = 0, completed_frames = 0, last_submission_cycles = 0;
    unsigned active_pixels = 0;
    bool host_rising = false, host_falling = false, write_accepted = false;
    std::function<void()> pump;

    explicit Simulation(const std::string& trace_path) {
        dut.clk = 0; dut.reset = 1; dut.host_clk = 0; dut.abort_request = 0;
        dut.host_wr_valid = 0; dut.host_wr_data = 0; dut.host_rd_req = 0; dut.host_reg_addr = 0;
        dut.sd_dq_in = 0xcdcdcdcdu;
        if (!trace_path.empty()) {
            context.traceEverOn(true); trace = std::make_unique<VerilatedVcdC>();
            dut.trace(trace.get(), 8); trace->open(trace_path.c_str());
        }
        for (unsigned edge = 0; edge < 16; ++edge) tick(false);
        dut.reset = 0;
        wait([&] { return dut.ready; }, "SDRAM initialization and attachment boot");
    }
    ~Simulation() { if (trace) trace->close(); dut.final(); }

    void tick(bool check_fault = true) {
        dut.clk = 0; dut.eval(); context.timeInc(1); if (trace) trace->dump(context.time());
        const bool next_host = (cycles & 3u) >= 2u;
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
            if (dut.video_x == 0 && dut.video_y == 0) {
                if (active_pixels == screen_width * screen_height) { display = working; ++completed_frames; }
                active_pixels = 0;
            }
            if (dut.video_de && dut.video_x < screen_width && dut.video_y < screen_height) {
                working[dut.video_y * screen_width + dut.video_x] = dut.video_rgb565;
                ++active_pixels;
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
                    if (hw::read_u32(result, 0) != 0x5550474e || result[4] != 0 || result[5] != 1 ||
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
        const auto start = cycles; send(program.bytes);
        const auto response = reply(hw::Opcode::Fence, program.fence_sequence);
        if (hw::read_u32(response, 24) != program.fence_token) throw std::runtime_error("wrong RTL fence token");
        last_submission_cycles = cycles - start;
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
    void present(int target, std::uint32_t sequence) {
        std::vector<std::uint8_t> payload(8);
        hw::write_u32(payload, 0, target); hw::write_u32(payload, 4, sequence);
        send(hw::encode_packet(hw::Opcode::Present, sequence, payload));
        (void)reply(hw::Opcode::Present, sequence);
        // Capture a complete new-front frame; the first completed observation
        // after the reply can still belong to active pixels before the swap.
        const auto before = completed_frames;
        wait([&] { return completed_frames >= before + 2; }, "complete actual scanout");
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
        std::vector<std::uint8_t> payload(8 + bytes.size());
        hw::write_u32(payload, 0, address); hw::write_u32(payload, 4, bytes.size());
        std::copy(bytes.begin(), bytes.end(), payload.begin() + 8);
        simulation.send(hw::encode_packet(hw::Opcode::WriteHeap, sequence++, payload));
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

    Renderer oracle;
    Pass pass; pass.clear_color = 0x18e3;
    pass.primitives.push_back(Rect{{3, 5, 31, 25}, overlay_state({241, 63, 27}), 0});
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

    pass.color_load = LoadOp::Load; pass.depth_load = LoadOp::Load; pass.primitives.clear();
    pass.primitives.push_back(Line{{0, 0}, {639, 479}, overlay_state({17, 191, 97}), 0});
    program = hw::compile_pass(pass, sequence); sequence = program.next_sequence;
    simulation.render(program); oracle.render(pass, RenderMode::Monolithic); simulation.compare(oracle);
    simulation.present(1, sequence++); oracle.queue_present(1); oracle.vertical_blank();
    require(simulation.display == oracle.front_color(), "post-copy/LOAD scanout");
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
    for (unsigned index = 0; index < screen_width * screen_height; ++index) {
        const auto word = simulation.memory.inspect_word(hw::color_buffer_base(1) + (index & ~1u) * 2);
        require(static_cast<std::uint16_t>(word >> (16 * (index & 1u))) == oracle.front_color()[index],
                "active abort preserves every front-buffer pixel");
    }
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
        Simulation simulation(options.trace); Renderer oracle;
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
            simulation.present(target, sequence++);
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
        std::ostringstream image_hash; image_hash << std::hex << std::setfill('0') << std::setw(16) << hash(simulation.display);
        std::string report = "{\n  \"profile\": \"N20K-RTL-SIM-0.1\",\n  \"backend\": \"verilator\",\n"
            "  \"cpp_device_backends_linked\": false,\n  \"whole_gpu_fit_qualified\": false,\n  \"frames\": " + std::to_string(frames) +
            ",\n  \"verified\": " + (options.verify ? "true" : "false") + ",\n  \"core_cycles\": " + std::to_string(simulation.cycles) +
            ",\n  \"last_submission_cycles\": " + std::to_string(simulation.last_submission_cycles) +
            ",\n  \"wall_seconds_excluding_boot\": " + std::to_string(wall) +
            ",\n  \"color_fnv1a64\": \"" + image_hash.str() + "\",\n  \"sdram_refreshes\": " +
            std::to_string(simulation.memory.statistics().refreshes) + "\n}\n";
        std::cout << report;
        if (!options.stats.empty()) { std::ofstream out(options.stats); if (!out) throw std::runtime_error("cannot write statistics"); out << report; }
        if (!options.output.empty()) ppm(options.output, simulation.display);
        return 0;
    } catch (const WindowClosed&) { return 0; }
    catch (const std::exception& error) { std::cerr << "nano-gpu-rtl: " << error.what() << '\n'; return 1; }
}
