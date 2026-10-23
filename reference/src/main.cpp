#include "nano_gpu/demo.hpp"
#include "nano_gpu/reference.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef NANO_GPU_WITH_SDL
#include <SDL.h>
#endif

namespace {
using namespace nano_gpu;
using Clock = std::chrono::steady_clock;

struct Options {
    bool help = false;
    bool headless = false;
    bool verify = false;
    std::uint64_t frames = 0; // Zero means an interactive window until closed.
    long double start_time = 0;
    long double time_step = 1.0L / 60.0L;
    DemoScene scene = DemoScene::Showcase;
    RenderMode mode = RenderMode::Tiled;
    std::string output;
    std::string dump_prefix;
    std::string stats_path;
};

void usage() {
    std::cout <<
        "Nano-20K-GPU software reference renderer (N20K-M0-0.1)\n"
        "Usage: nano-gpu-ref [options]\n"
        "  --headless                 Render without initializing SDL (default: one frame)\n"
        "  --scene NAME               showcase, perspective, or overdraw\n"
        "  --frames N                 Render N frames; window default runs until closed\n"
        "  --time SECONDS             Initial deterministic animation time (default: 0)\n"
        "  --step SECONDS             Time advance per frame (default: 1/60; zero freezes)\n"
        "  --mode MODE                tiled, reverse, or monolithic\n"
        "  --verify                   Compare every frame with both other traversal modes\n"
        "  --output FILE.ppm          Save the last presented RGB frame\n"
        "  --dump-prefix PATH         Save PATH.rgb565, PATH.depth32, PATH.json\n"
        "  --stats FILE.json          Save software timings and last-frame counters\n"
        "  --help                     Show help\n"
        "Window controls: Escape/Q closes, Space pauses, S saves build/screenshot.ppm.\n";
}

long double number(const std::string& input, const std::string& name) {
    std::size_t consumed = 0;
    const long double result = std::stold(input, &consumed);
    if (consumed != input.size() || !std::isfinite(result) || result < 0)
        throw std::invalid_argument(name + " must be a finite nonnegative number");
    return result;
}

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc) throw std::invalid_argument("missing value after " + arg);
            return argv[i];
        };
        if (arg == "--help" || arg == "-h") options.help = true;
        else if (arg == "--headless") options.headless = true;
        else if (arg == "--verify") options.verify = true;
        else if (arg == "--output") options.output = value();
        else if (arg == "--dump-prefix") options.dump_prefix = value();
        else if (arg == "--stats") options.stats_path = value();
        else if (arg == "--time") options.start_time = number(value(), arg);
        else if (arg == "--step") options.time_step = number(value(), arg);
        else if (arg == "--frames") {
            const std::string input = value();
            if (input.empty() || input.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("--frames must be a positive integer");
            options.frames = std::stoull(input);
            if (options.frames == 0 || options.frames > 1000000)
                throw std::invalid_argument("--frames must be in 1..1000000");
        } else if (arg == "--scene") {
            const std::string name = value();
            if (name == "showcase") options.scene = DemoScene::Showcase;
            else if (name == "perspective") options.scene = DemoScene::Perspective;
            else if (name == "overdraw") options.scene = DemoScene::Overdraw;
            else throw std::invalid_argument("unknown scene: " + name);
        } else if (arg == "--mode") {
            const std::string name = value();
            if (name == "tiled") options.mode = RenderMode::Tiled;
            else if (name == "reverse") options.mode = RenderMode::TiledReverse;
            else if (name == "monolithic") options.mode = RenderMode::Monolithic;
            else throw std::invalid_argument("unknown traversal mode: " + name);
        } else throw std::invalid_argument("unknown option: " + arg);
    }
    if (options.headless && options.frames == 0) options.frames = 1;
    return options;
}

std::ofstream output_file(const std::string& name, bool binary = false) {
    const auto parent = std::filesystem::path(name).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream out(name, std::ios::out | (binary ? std::ios::binary : std::ios::openmode{}));
    if (!out) throw std::runtime_error("cannot open output: " + name);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    return out;
}

void write_ppm(const std::string& name, const std::vector<std::uint16_t>& pixels) {
    auto out = output_file(name, true);
    out << "P6\n" << screen_width << ' ' << screen_height << "\n255\n";
    for (const auto value : pixels) {
        const Color color = expand_rgb565(value);
        const char bytes[] = {static_cast<char>(color.r), static_cast<char>(color.g), static_cast<char>(color.b)};
        out.write(bytes, 3);
    }
    out.close();
}

template<class T>
std::uint64_t buffer_hash(const std::vector<T>& pixels) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto pixel : pixels)
        for (unsigned byte = 0; byte < sizeof(T); ++byte) {
            hash ^= (static_cast<std::uint64_t>(pixel) >> (8 * byte)) & 255;
            hash *= 1099511628211ULL;
        }
    return hash;
}

void write_dumps(const std::string& prefix, const Renderer& renderer,
                 const Options& options, long double time) {
    {
        auto out = output_file(prefix + ".rgb565", true);
        for (const auto pixel : renderer.front_color()) {
            out.put(static_cast<char>(pixel & 255));
            out.put(static_cast<char>(pixel >> 8));
        }
        out.close();
    }
    {
        auto out = output_file(prefix + ".depth32", true);
        for (const auto pixel : renderer.depth())
            for (unsigned byte = 0; byte < 4; ++byte)
                out.put(static_cast<char>((pixel >> (8 * byte)) & 255));
        out.close();
    }
    auto out = output_file(prefix + ".json");
    out << "{\n  \"profile\": \"N20K-M0-0.1\",\n  \"scene\": \"" << scene_name(options.scene)
        << "\",\n  \"time_seconds\": " << std::setprecision(18) << time
        << ",\n  \"width\": 640,\n  \"height\": 480,\n  \"tile_size\": 32,"
        << "\n  \"byte_order\": \"little\",\n  \"color_format\": \"RGB565\","
        << "\n  \"color_pitch_bytes\": 1280,\n  \"depth_format\": \"D24_IN_U32\","
        << "\n  \"depth_pitch_bytes\": 2560,\n  \"color_fnv1a64\": \"" << std::hex
        << buffer_hash(renderer.front_color()) << "\",\n  \"depth_fnv1a64\": \""
        << buffer_hash(renderer.depth()) << "\"\n}\n";
    out.close();
}

void verify_frame(const Pass& input, const Renderer& actual, RenderMode selected) {
    for (const auto mode : {RenderMode::Tiled, RenderMode::TiledReverse, RenderMode::Monolithic}) {
        if (mode == selected) continue;
        Renderer expected;
        Pass pass = input;
        pass.target = expected.back_id();
        expected.render(pass, mode);
        if (expected.color(pass.target) != actual.color(input.target) || expected.depth() != actual.depth())
            throw std::runtime_error("tile/monolithic validation mismatch");
    }
}

struct Timings {
    std::uint64_t frames = 0;
    double setup_ms = 0;
    double render_ms = 0;
    double verify_ms = 0;
};

void report(std::ostream& out, const Renderer& renderer, const Options& options, const Timings& timing) {
    const auto& s = renderer.statistics();
    const char* mode = options.mode == RenderMode::Tiled ? "tiled" :
                       options.mode == RenderMode::TiledReverse ? "reverse" : "monolithic";
    out << std::setprecision(9) << "{\n  \"profile\": \"N20K-M0-0.1\",\n  \"scene\": \""
        << scene_name(options.scene) << "\",\n  \"mode\": \"" << mode
        << "\",\n  \"frames\": " << timing.frames
        << ",\n  \"verified_against_other_modes\": " << (options.verify ? "true" : "false")
        << ",\n  \"mean_setup_ms\": " << timing.setup_ms / static_cast<double>(timing.frames)
        << ",\n  \"mean_render_and_bin_ms\": " << timing.render_ms / static_cast<double>(timing.frames)
        << ",\n  \"mean_verification_ms\": " << timing.verify_ms / static_cast<double>(timing.frames)
        << ",\n  \"last_frame\": {\n    \"primitives_binned_references\": " << s.tile_references
        << ",\n    \"tiles_processed\": " << s.tiles_processed
        << ",\n    \"candidate_positions\": " << s.candidates
        << ",\n    \"covered_fragments\": " << s.covered_fragments
        << ",\n    \"depth_rejected\": " << s.depth_rejected
        << ",\n    \"keyed_rejected\": " << s.keyed_rejected
        << ",\n    \"texture_samples\": " << s.texture_samples
        << ",\n    \"color_writes\": " << s.color_writes
        << ",\n    \"depth_writes\": " << s.depth_writes
        << "\n  },\n  \"color_fnv1a64\": \"" << std::hex << buffer_hash(renderer.front_color())
        << "\",\n  \"depth_fnv1a64\": \"" << buffer_hash(renderer.depth()) << "\"\n}\n" << std::dec;
}

#ifdef NANO_GPU_WITH_SDL
struct SdlSession {
    SdlSession() {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
    }
    ~SdlSession() { SDL_Quit(); }
};

class Presenter {
public:
    explicit Presenter(bool finite) {
        const std::string driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "";
        if (!finite && (driver == "dummy" || driver == "offscreen"))
            throw std::runtime_error("no visible video backend; use --headless or a finite --frames run");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        window_.reset(SDL_CreateWindow("Nano-20K GPU reference | Space: pause | S: save | Esc: quit",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960, SDL_WINDOW_RESIZABLE));
        if (!window_) throw std::runtime_error(SDL_GetError());
        renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
        if (!renderer_) renderer_.reset(SDL_CreateRenderer(window_.get(), -1, SDL_RENDERER_SOFTWARE));
        if (!renderer_ || SDL_RenderSetLogicalSize(renderer_.get(), screen_width, screen_height) != 0)
            throw std::runtime_error(SDL_GetError());
        texture_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGB565,
                                        SDL_TEXTUREACCESS_STREAMING, screen_width, screen_height));
        if (!texture_ || SDL_SetTextureBlendMode(texture_.get(), SDL_BLENDMODE_NONE) != 0)
            throw std::runtime_error(SDL_GetError());
    }
    bool events(bool& paused, const Renderer& renderer) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) return false;
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) return false;
                if (event.key.keysym.sym == SDLK_SPACE) paused = !paused;
                if (event.key.keysym.sym == SDLK_s) write_ppm("build/screenshot.ppm", renderer.front_color());
            }
        }
        return true;
    }
    void show(const Renderer& renderer) {
        if (SDL_UpdateTexture(texture_.get(), nullptr, renderer.front_color().data(), screen_width * 2) != 0 ||
            SDL_RenderClear(renderer_.get()) != 0 ||
            SDL_RenderCopy(renderer_.get(), texture_.get(), nullptr, nullptr) != 0)
            throw std::runtime_error(SDL_GetError());
        SDL_RenderPresent(renderer_.get());
    }
private:
    SdlSession session_;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window_{nullptr, SDL_DestroyWindow};
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer_{nullptr, SDL_DestroyRenderer};
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture_{nullptr, SDL_DestroyTexture};
};
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse(argc, argv);
        if (options.help) { usage(); return 0; }
#ifdef NANO_GPU_WITH_SDL
        std::unique_ptr<Presenter> presenter;
        if (!options.headless) presenter = std::make_unique<Presenter>(options.frames != 0);
#else
        if (!options.headless) throw std::invalid_argument("this build is headless; add --headless or build the SDL target with make");
#endif
        Renderer renderer;
        Timings timing;
        long double time = options.start_time;
        long double last_time = time;
        bool paused = false;
        while (options.frames == 0 || timing.frames < options.frames) {
            const auto start = Clock::now();
#ifdef NANO_GPU_WITH_SDL
            if (presenter && !presenter->events(paused, renderer)) break;
#endif
            Pass pass = make_demo_pass(options.scene, renderer.back_id(), time);
            const auto prepared = Clock::now();
            renderer.render(pass, options.mode);
            const auto rendered = Clock::now();
            if (options.verify) verify_frame(pass, renderer, options.mode);
            const auto verified = Clock::now();
            renderer.queue_present(pass.target);
            renderer.vertical_blank();
#ifdef NANO_GPU_WITH_SDL
            if (presenter) presenter->show(renderer);
#endif
            timing.setup_ms += std::chrono::duration<double, std::milli>(prepared - start).count();
            timing.render_ms += std::chrono::duration<double, std::milli>(rendered - prepared).count();
            timing.verify_ms += std::chrono::duration<double, std::milli>(verified - rendered).count();
            ++timing.frames;
            last_time = time;
            if (!paused) time += options.time_step;
#ifdef NANO_GPU_WITH_SDL
            if (presenter) {
                const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                if (elapsed < 16.0) SDL_Delay(static_cast<Uint32>(16.0 - elapsed));
            }
#endif
        }
        if (timing.frames == 0) return 0;
        if (!options.output.empty()) write_ppm(options.output, renderer.front_color());
        if (!options.dump_prefix.empty()) write_dumps(options.dump_prefix, renderer, options, last_time);
        if (!options.stats_path.empty()) {
            auto out = output_file(options.stats_path);
            report(out, renderer, options, timing);
            out.close();
        }
        report(std::cout, renderer, options, timing);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "nano-gpu-ref: " << error.what() << '\n';
        return 1;
    }
}
