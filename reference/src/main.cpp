#include "nano_gpu/demo.hpp"
#include "nano_gpu/reference.hpp"
#include "nano_gpu/hw/device.hpp"
#include "nano_gpu/hw/system.hpp"
#include "nano_gpu/hw/storage_map.hpp"
#include "nano_gpu/hw/throughput.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#ifdef NANO_GPU_WITH_SDL
#include <SDL.h>
#endif

namespace {
using namespace nano_gpu;
using Clock = std::chrono::steady_clock;
constexpr unsigned scanout_bsram_banks = 2;
constexpr unsigned reply_serializer_bsram_banks = 1;
constexpr unsigned system_native_bsram_banks = hw::SystemStorageMap::bsram_blocks;
static_assert(system_native_bsram_banks <= hw::TangNano20K::bsram_blocks);

struct Options {
    bool help = false;
    bool hardware_profile = false;
    bool headless = false;
    bool verify = false;
    bool system = false;
    bool system_options = false;
    hw::SystemConfig system_config{};
    std::uint64_t frames = 0; // Zero means an interactive window until closed.
    long double start_time = 0;
    long double time_step = 1.0L / 60.0L;
    DemoScene scene = DemoScene::Showcase;
    RenderMode mode = RenderMode::Tiled;
    std::string output;
    std::string dump_prefix;
    std::string stats_path;
    std::string trace_path;
};

const char* render_mode_name(RenderMode mode) {
    switch (mode) {
    case RenderMode::Tiled: return "tiled";
    case RenderMode::TiledReverse: return "reverse";
    case RenderMode::Monolithic: return "monolithic";
    case RenderMode::HardwareBlocks: return "blocks";
    }
    throw std::invalid_argument("unknown render mode");
}

const char* render_mode_name(const Options& options) {
    return options.system ? "system" : render_mode_name(options.mode);
}

void usage() {
    std::cout <<
        "Nano-20K-GPU software reference renderer (N20K-M0-0.1)\n"
        "Usage: nano-gpu-ref [options]\n"
        "  --headless                 Render without initializing SDL (default: one frame)\n"
        "  --scene NAME               showcase, perspective, overdraw, or opaque\n"
        "  --frames N                 Render N frames; window default runs until closed\n"
        "  --time SECONDS             Initial deterministic animation time (default: 0)\n"
        "  --step SECONDS             Time advance per frame (default: 1/60; zero freezes)\n"
        "  --mode MODE                tiled, reverse, monolithic, blocks, or system\n"
        "  --hardware-profile         Show verified device limits and modeled subset\n"
        "  --verify                   Compare every frame with canonical traversal modes\n"
        "  --output FILE.ppm          Save the last presented RGB frame\n"
        "  --dump-prefix PATH         Save PATH.rgb565, PATH.depth32, PATH.json\n"
        "  --stats FILE.json          Save software timings and last-frame counters\n"
        "  --trace FILE.csv           Stream every system core edge and fragment events\n"
        "System-mode timing assumptions and stress options:\n"
        "  --core-hz HZ               Common core/SDR clock (default: 100800000)\n"
        "  --host-divider N           Host clock = core/N (default: 4)\n"
        "  --sdram-trcd N             Activate-to-column delay in core cycles (default: 3)\n"
        "  --sdram-trp N              Precharge delay; raises tRC as needed (default: 3)\n"
        "  --sdram-trfc N             Refresh delay in core cycles (default: 9)\n"
        "  --scanout-fifo N           Display FIFO pixels, power of two 64..2048 (default: 2048)\n"
        "System pixel clock = core/4; default 800x525 timing gives 60 Hz.\n"
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

std::uint64_t positive_integer(const std::string& input, const std::string& name,
                               std::uint64_t maximum) {
    if (input.empty() || input.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument(name + " must be a positive integer");
    const auto result = std::stoull(input);
    if (result == 0 || result > maximum)
        throw std::invalid_argument(name + " is outside its supported range");
    return result;
}

bool same_output_path(const std::string& first, const std::string& second) {
    namespace fs = std::filesystem;
    // Existing hard links and symlinks can alias even when their names differ.
    std::error_code error;
    if (fs::equivalent(fs::path(first), fs::path(second), error)) return true;
    const auto normalized = [](const std::string& name) {
        return fs::weakly_canonical(fs::absolute(fs::path(name))).lexically_normal();
    };
    const auto first_path = normalized(first), second_path = normalized(second);
#ifdef _WIN32
    // Native Windows output paths normally ignore case, including filenames
    // that do not exist yet. Native path strings are UTF-16 on MinGW/UCRT64.
    auto first_name = first_path.native(), second_name = second_path.native();
    for (auto& character : first_name) character = static_cast<wchar_t>(std::towlower(character));
    for (auto& character : second_name) character = static_cast<wchar_t>(std::towlower(character));
    return first_name == second_name;
#else
    return first_path == second_path;
#endif
}

void validate_trace_outputs(const Options& options) {
    if (options.trace_path.empty()) return;
    const auto require_distinct = [&](const std::string& name, const char* option) {
        if (!name.empty() && same_output_path(options.trace_path, name))
            throw std::invalid_argument(std::string("--trace path collides with ") + option + " output");
    };
    require_distinct(options.stats_path, "--stats");
    require_distinct(options.output, "--output");
    if (!options.dump_prefix.empty()) {
        require_distinct(options.dump_prefix + ".json", "--dump-prefix .json");
        require_distinct(options.dump_prefix + ".rgb565", "--dump-prefix .rgb565");
        require_distinct(options.dump_prefix + ".depth32", "--dump-prefix .depth32");
    }
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
        else if (arg == "--hardware-profile") options.hardware_profile = true;
        else if (arg == "--headless") options.headless = true;
        else if (arg == "--verify") options.verify = true;
        else if (arg == "--output") options.output = value();
        else if (arg == "--dump-prefix") options.dump_prefix = value();
        else if (arg == "--stats") options.stats_path = value();
        else if (arg == "--trace") options.trace_path = value();
        else if (arg == "--time") options.start_time = number(value(), arg);
        else if (arg == "--step") options.time_step = number(value(), arg);
        else if (arg == "--core-hz") {
            options.system_config.memory.core_hz = positive_integer(value(), arg,
                std::numeric_limits<std::uint64_t>::max());
            options.system_options = true;
        } else if (arg == "--host-divider") {
            const auto divider = positive_integer(value(), arg, std::numeric_limits<unsigned>::max());
            options.system_config.host.clock_divider = static_cast<unsigned>(divider);
            options.system_config.host.transfer_phase = static_cast<unsigned>(divider - 1);
            options.system_options = true;
        } else if (arg == "--sdram-trcd" || arg == "--sdram-trp" || arg == "--sdram-trfc" ||
                   arg == "--scanout-fifo") {
            const auto selected = static_cast<unsigned>(positive_integer(value(), arg,
                std::numeric_limits<unsigned>::max()));
            if (arg == "--sdram-trcd") options.system_config.memory.t_rcd = selected;
            else if (arg == "--sdram-trp") options.system_config.memory.t_rp = selected;
            else if (arg == "--sdram-trfc") options.system_config.memory.t_rfc = selected;
            else options.system_config.scanout.fifo_pixels = selected;
            options.system_options = true;
        } else if (arg == "--frames") {
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
            else if (name == "opaque") options.scene = DemoScene::Opaque;
            else throw std::invalid_argument("unknown scene: " + name);
        } else if (arg == "--mode") {
            const std::string name = value();
            options.system = name == "system";
            if (name == "tiled") options.mode = RenderMode::Tiled;
            else if (name == "reverse") options.mode = RenderMode::TiledReverse;
            else if (name == "monolithic") options.mode = RenderMode::Monolithic;
            else if (name == "blocks") options.mode = RenderMode::HardwareBlocks;
            else if (name == "system") options.mode = RenderMode::HardwareBlocks;
            else throw std::invalid_argument("unknown traversal mode: " + name);
        } else throw std::invalid_argument("unknown option: " + arg);
    }
    if (options.headless && options.frames == 0) options.frames = 1;
    if (options.system_options && !options.system)
        throw std::invalid_argument("clock, SDRAM timing, and scanout FIFO options require --mode system");
    if (!options.trace_path.empty() && !options.system)
        throw std::invalid_argument("--trace requires --mode system");
    validate_trace_outputs(options);
    const auto minimum_row_cycle = static_cast<std::uint64_t>(options.system_config.memory.t_ras) +
        options.system_config.memory.t_rp;
    if (minimum_row_cycle > std::numeric_limits<unsigned>::max())
        throw std::invalid_argument("--sdram-trp exceeds the row-cycle counter range");
    if (options.system_config.memory.t_rc < minimum_row_cycle)
        options.system_config.memory.t_rc = static_cast<unsigned>(minimum_row_cycle);
    return options;
}

void show_hardware_profile(const Options& options) {
    using Device = nano_gpu::hw::TangNano20K;
    using Slice = nano_gpu::hw::OpaqueSliceResources;
    std::cout << "{\n  \"part\": \"" << Device::part
        << "\",\n  \"architecture\": \"" << Device::architecture
        << "\",\n  \"lut4\": " << Device::lut4
        << ",\n  \"flip_flops\": " << Device::flip_flops
        << ",\n  \"bsram_blocks\": " << Device::bsram_blocks
        << ",\n  \"bsram_bits_per_block\": " << Device::bsram_bits_per_block
        << ",\n  \"mult18_equivalents\": " << Device::mult18_equivalents
        << ",\n  \"dsp_macros\": " << Device::dsp_macros
        << ",\n  \"dsp_blocks\": " << Device::dsp_blocks
        << ",\n  \"available_plls\": " << Device::available_plls
        << ",\n  \"sdram_bytes\": " << Device::sdram_bytes
        << ",\n  \"sdram_data_bits\": " << Device::sdram_data_bits
        << ",\n  \"selected_slice\": {\n    \"profile\": \"" << Slice::profile
        << "\",\n    \"resident_tiles\": " << Slice::resident_tiles
        << ",\n    \"bsram_blocks\": " << Slice::bsram_blocks
        << ",\n    \"tile_bsram_blocks\": " << Slice::tile_bsram_blocks
        << ",\n    \"texture_cache_banks\": " << Slice::texture_cache_banks
        << ",\n    \"mult18_equivalents\": " << Slice::mult18_equivalents
        << ",\n    \"multiplier\": \"signed 18x18, 36-bit result, three enabled edges\","
        << "\n    \"memory\": \"four SDPB 1024x16: three tile banks and one texture cache\","
        << "\n    \"perspective\": \"one 92-edge serial LUT divider; exact Q16 round-to-nearest-even\","
        << "\n    \"coverage\": \"all M0 shading, perspective textures, D24, 2D and ordered copies\""
        << "\n  },\n  \"system_model\": ";
    if (options.system) {
        std::cout << "{\n    \"profile\": \"N20K-SYS-0.1\","
            << "\n    \"configured_core_and_sdram_hz\": " << options.system_config.memory.core_hz
            << ",\n    \"host_data_bits\": 8,\n    \"host_clock_divider\": "
            << options.system_config.host.clock_divider
            << ",\n    \"pixel_clock_divider\": " << options.system_config.scanout.pixel_divider
            << ",\n    \"scanout_fifo_pixels\": " << options.system_config.scanout.fifo_pixels
            << ",\n    \"native_bsram_blocks\": " << system_native_bsram_banks
            << ",\n    \"tile_bsram_banks\": " << Slice::tile_bsram_blocks
            << ",\n    \"texture_and_copy_bsram_banks\": " << Slice::texture_cache_banks
            << ",\n    \"scanout_bsram_banks\": " << scanout_bsram_banks
            << ",\n    \"host_bsram_banks\": " << hw::HostLink::storage_bsram_blocks
            << ",\n    \"reply_serializer_bsram_banks\": " << reply_serializer_bsram_banks
            << ",\n    \"sdram_slot_bsram_banks\": " << hw::SystemStorageMap::sdram_slot_bsram_blocks
            << ",\n    \"reader_bsram_banks\": " << hw::SystemStorageMap::reader_bsram_blocks
            << ",\n    \"row_bsram_banks\": " << hw::SystemStorageMap::row_bsram_blocks
            << ",\n    \"storage_profile\": \"" << hw::SystemStorageMap::profile << "\","
            << "\n    \"staging_mapping_selected\": true,"
            << "\n    \"additional_staging_fit_validated\": false,"
            << "\n    \"capability_register_first\": 64,\n    \"capability_register_last\": 127,"
            << "\n    \"shared_memory_clients\": 6,"
            << "\n    \"sdram_geometry\": \"4 banks x 2048 rows x 256 columns x 32 bits\","
            << "\n    \"timing_profile\": \"configurable project assumptions\","
            << "\n    \"hdmi_signaling_modeled\": false\n  }";
    } else std::cout << "null";
    std::cout << ",\n  \"physical_implementation_validated\": false\n}\n";
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

template<class Engine>
void write_dumps(const std::string& prefix, const Engine& renderer,
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
        << "\",\n  \"mode\": \"" << render_mode_name(options)
        << "\",\n  \"hardware_slice_profile\": ";
    if (options.system) out << "\"N20K-SYS-0.1\"";
    else if (options.mode == RenderMode::HardwareBlocks)
        out << '"' << hw::OpaqueSliceResources::profile << '"';
    else out << "null";
    out << ",\n  \"time_seconds\": " << std::setprecision(18) << time
        << ",\n  \"width\": 640,\n  \"height\": 480,\n  \"tile_size\": 32,"
        << "\n  \"byte_order\": \"little\",\n  \"color_format\": \"RGB565\","
        << "\n  \"color_pitch_bytes\": 1280,\n  \"depth_format\": \"D24_IN_U32\","
        << "\n  \"depth_pitch_bytes\": 2560,\n  \"color_fnv1a64\": \"" << std::hex
        << buffer_hash(renderer.front_color()) << "\",\n  \"depth_fnv1a64\": \""
        << buffer_hash(renderer.depth()) << "\"\n}\n";
    out.close();
}

template<class Engine>
void verify_frame(const Pass& input, const Engine& actual, RenderMode selected,
                  std::array<Renderer, 3>& references) {
    constexpr std::array<RenderMode, 3> modes{
        RenderMode::Tiled, RenderMode::TiledReverse, RenderMode::Monolithic};
    for (std::size_t index = 0; index < modes.size(); ++index) {
        const auto mode = modes[index];
        if (mode == selected) continue;
        auto& expected = references[index];
        expected.render(input, mode);
        if (expected.color(0) != actual.color(0) || expected.color(1) != actual.color(1) ||
            expected.depth() != actual.depth())
            throw std::runtime_error("canonical traversal color/depth validation mismatch");
        expected.queue_present(input.target);
        expected.vertical_blank();
    }
}

struct Timings {
    std::uint64_t frames = 0;
    double setup_ms = 0;
    double render_ms = 0;
    double verify_ms = 0;
    double present_ms = 0;
};

void report_system(std::ostream& out, const hw::SystemModel& renderer) {
    const auto& config = renderer.config();
    const auto& s = renderer.system_statistics();
    const auto& memory = renderer.memory_statistics();
    const auto& host = renderer.host_statistics();
    const auto& scanout = renderer.scanout_statistics();
    const auto core_hz = config.memory.core_hz;
    const auto horizontal_total = screen_width + config.scanout.horizontal_front_porch +
        config.scanout.horizontal_sync + config.scanout.horizontal_back_porch;
    const auto vertical_total = screen_height + config.scanout.vertical_front_porch +
        config.scanout.vertical_sync + config.scanout.vertical_back_porch;
    out << ",\n  \"system\": {\n    \"profile\": \"N20K-SYS-0.1\","
        << "\n    \"timing_profile\": \"configurable project assumptions\","
        << "\n    \"physical_timing_validated\": false,"
        << "\n    \"hdmi_signaling_modeled\": false,"
        << "\n    \"native_bsram_blocks\": " << system_native_bsram_banks
        << ",\n    \"storage_profile\": \"" << hw::SystemStorageMap::profile << "\","
        << "\n    \"staging_mapping_selected\": true,"
        << "\n    \"additional_staging_fit_validated\": false,"
        << "\n    \"configured_core_hz\": " << core_hz
        << ",\n    \"configured_sdram_hz\": " << core_hz
        << ",\n    \"configured_host_hz\": " << static_cast<double>(core_hz) / config.host.clock_divider
        << ",\n    \"configured_pixel_hz\": " << static_cast<double>(core_hz) / config.scanout.pixel_divider
        << ",\n    \"configured_refresh_hz\": " << static_cast<double>(core_hz) /
            config.scanout.pixel_divider / horizontal_total / vertical_total
        << ",\n    \"core_cycles\": " << s.core_cycles
        << ",\n    \"simulated_seconds\": " << static_cast<double>(s.core_cycles) / core_hz
        << ",\n    \"boot_cycles\": " << s.boot_cycles
        << ",\n    \"last_submission_cycles\": " << s.last_submission_cycles
        << ",\n    \"last_submission_simulated_seconds\": " <<
            static_cast<double>(s.last_submission_cycles) / core_hz
        << ",\n    \"last_pass_cycles\": " << s.last_pass_cycles
        << ",\n    \"last_present_wait_cycles\": " << s.last_present_wait_cycles
        << ",\n    \"last_scanout_observation_cycles\": " << s.last_scanout_observation_cycles
        << ",\n    \"commands_completed\": " << s.commands_completed
        << ",\n    \"passes_completed\": " << s.passes_completed
        << ",\n    \"copies_completed\": " << s.copies_completed
        << ",\n    \"presentations_completed\": " << s.presentations_completed
        << ",\n    \"uploaded_bytes\": " << s.uploaded_bytes
        << ",\n    \"fetched_record_bytes\": " << s.fetched_record_bytes
        << ",\n    \"fetched_bin_bytes\": " << s.fetched_bin_bytes
        << ",\n    \"epoch\": " << s.epoch
        << ",\n    \"completed_sequence\": " << s.completed_sequence
        << ",\n    \"completed_fence_token\": " << s.completed_fence_token
        << ",\n    \"completed_present_token\": " << s.completed_present_token
        << ",\n    \"aborts\": " << s.aborts
        << ",\n    \"front_buffer\": " << renderer.front_id()
        << ",\n    \"presentation_pending\": " << (renderer.presentation_pending() ? "true" : "false")
        << ",\n    \"sdram\": {\n      \"cas_latency\": " << config.memory.cas_latency
        << ",\n      \"t_rcd\": " << config.memory.t_rcd
        << ",\n      \"t_rp\": " << config.memory.t_rp
        << ",\n      \"t_rfc\": " << config.memory.t_rfc
        << ",\n      \"t_mrd\": " << config.memory.t_mrd
        << ",\n      \"t_wr\": " << config.memory.t_wr
        << ",\n      \"t_ras\": " << config.memory.t_ras
        << ",\n      \"t_rc\": " << config.memory.t_rc
        << ",\n      \"read_to_write\": " << config.memory.read_to_write
        << ",\n      \"write_to_read\": " << config.memory.write_to_read
        << ",\n      \"power_up_cycles\": " << (config.memory.power_up_cycles ?
            config.memory.power_up_cycles : (core_hz + 9999) / 10000)
        << ",\n      \"refresh_interval_cycles\": " << (config.memory.refresh_interval_cycles ?
            config.memory.refresh_interval_cycles : core_hz / 64000)
        << ",\n      \"scanout_priority_limit\": " << config.memory.scanout_priority_limit
        << ",\n      \"initialization_cycles\": " << memory.initialization_cycles
        << ",\n      \"refreshes\": " << memory.refreshes
        << ",\n      \"refresh_busy_cycles\": " << memory.refresh_busy_cycles
        << ",\n      \"refresh_late_cycles\": " << memory.refresh_late_cycles
        << ",\n      \"max_refresh_gap_cycles\": " << memory.max_refresh_gap_cycles
        << ",\n      \"row_hits\": " << memory.row_hits
        << ",\n      \"row_misses\": " << memory.row_misses
        << ",\n      \"activates\": " << memory.activates
        << ",\n      \"precharges\": " << memory.precharges
        << ",\n      \"read_commands\": " << memory.read_commands
        << ",\n      \"write_commands\": " << memory.write_commands
        << ",\n      \"physical_read_beats\": " << memory.physical_read_beats
        << ",\n      \"physical_write_beats\": " << memory.physical_write_beats
        << ",\n      \"uninitialized_read_words\": " << memory.uninitialized_read_words
        << ",\n      \"idle_cycles\": " << memory.idle_cycles
        << ",\n      \"prepared_submissions\": " << memory.prepared_submissions
        << ",\n      \"legacy_submissions\": " << memory.legacy_submissions
        << ",\n      \"legacy_response_reads\": " << memory.legacy_response_reads
        << ",\n      \"slot_write_words\": " << memory.slot_write_words
        << ",\n      \"slot_read_words\": " << memory.slot_read_words
        << ",\n      \"clients\": {";
    constexpr std::array<const char*, hw::memory_client_count> names{
        "scanout", "host", "command", "tile", "texture", "copy"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto& client = memory.clients[index];
        if (index != 0) out << ',';
        out << "\n        \"" << names[index] << "\": {\"requests\": " << client.requests
            << ", \"completed\": " << client.completed
            << ", \"bytes_read\": " << client.bytes_read
            << ", \"bytes_written\": " << client.bytes_written
            << ", \"queue_wait_cycles\": " << client.queue_wait_cycles
            << ", \"max_queue_wait_cycles\": " << client.max_queue_wait_cycles
            << ", \"service_cycles\": " << client.service_cycles
            << ", \"max_service_cycles\": " << client.max_service_cycles
            << ", \"rejected_busy\": " << client.rejected_busy << '}';
    }
    out << "\n      }\n    },\n    \"host\": {\n      \"data_bits\": 8,"
        << "\n      \"clock_divider\": " << config.host.clock_divider
        << ",\n      \"transfer_phase\": " << config.host.transfer_phase
        << ",\n      \"setup_cycles\": " << config.host.setup_cycles
        << ",\n      \"hold_cycles\": " << config.host.hold_cycles
        << ",\n      \"rx_fifo_capacity\": " << config.host.rx_fifo_capacity
        << ",\n      \"command_fifo_capacity\": " << config.host.command_fifo_capacity
        << ",\n      \"response_fifo_capacity\": " << config.host.response_fifo_capacity
        << ",\n      \"rx_bytes\": " << host.rx_bytes
        << ",\n      \"tx_bytes\": " << host.tx_bytes
        << ",\n      \"status_reads\": " << host.status_reads
        << ",\n      \"capability_reads\": " << host.capability_reads
        << ",\n      \"backpressure_cycles\": " << host.backpressure_cycles
        << ",\n      \"backpressure_edges\": " << host.backpressure_edges
        << ",\n      \"parser_stall_cycles\": " << host.parser_stall_cycles
        << ",\n      \"commands\": " << host.commands
        << ",\n      \"protocol_faults\": " << host.protocol_faults
        << ",\n      \"setup_violations\": " << host.setup_violations
        << ",\n      \"hold_violations\": " << host.hold_violations
        << ",\n      \"unstable_writes\": " << host.unstable_writes
        << ",\n      \"bus_direction_violations\": " << host.bus_direction_violations
        << ",\n      \"rx_high_water\": " << host.rx_high_water
        << ",\n      \"command_high_water\": " << host.command_high_water
        << ",\n      \"response_high_water\": " << host.response_high_water
        << "\n    },\n    \"scanout\": {\n      \"pixel_divider\": " << config.scanout.pixel_divider
        << ",\n      \"fifo_pixels\": " << config.scanout.fifo_pixels
        << ",\n      \"horizontal_total\": " << horizontal_total
        << ",\n      \"vertical_total\": " << vertical_total
        << ",\n      \"timing_frames\": " << scanout.frames
        << ",\n      \"completed_frames\": " << scanout.completed_frames
        << ",\n      \"framebuffer_swaps\": " << scanout.framebuffer_swaps
        << ",\n      \"timing_frames_without_swap\": " << (scanout.frames >= scanout.framebuffer_swaps ?
            scanout.frames - scanout.framebuffer_swaps : 0)
        << ",\n      \"active_pixels\": " << scanout.active_pixels
        << ",\n      \"displayed_pixels\": " << scanout.displayed_pixels
        << ",\n      \"startup_black_pixels\": " << scanout.startup_black_pixels
        << ",\n      \"underflows\": " << scanout.underflows
        << ",\n      \"underflow_pixels\": " << scanout.underflow_pixels
        << ",\n      \"read_requests\": " << scanout.read_requests
        << ",\n      \"stale_responses\": " << scanout.stale_responses
        << ",\n      \"fifo_high_water\": " << scanout.fifo_high_water
        << ",\n      \"last_completed_frame_fnv1a64\": \"" << std::hex
        << buffer_hash(renderer.display_color()) << "\"\n    }\n  }" << std::dec;
}

template<class Engine>
void report(std::ostream& out, const Engine& renderer, const Options& options, const Timings& timing,
            const hw::ThroughputMonitor* throughput = nullptr) {
    const auto& s = renderer.statistics();
    const char* mode = render_mode_name(options);
    out << std::setprecision(9) << "{\n  \"profile\": \"N20K-M0-0.1\",\n  \"scene\": \""
        << scene_name(options.scene) << "\",\n  \"mode\": \"" << mode
        << "\",\n  \"frames\": " << timing.frames
        << ",\n  \"verified_against_other_modes\": " << (options.verify ? "true" : "false")
        << ",\n  \"mean_setup_ms\": " << timing.setup_ms / static_cast<double>(timing.frames)
        << ",\n  \"mean_render_and_bin_ms\": " << timing.render_ms / static_cast<double>(timing.frames)
        << ",\n  \"mean_verification_ms\": " << timing.verify_ms / static_cast<double>(timing.frames)
        << ",\n  \"mean_present_and_scanout_ms\": " << timing.present_ms / static_cast<double>(timing.frames)
        << ",\n  \"last_frame\": {\n    \"primitives_binned_references\": " << s.tile_references
        << ",\n    \"tiles_processed\": " << s.tiles_processed
        << ",\n    \"candidate_positions\": " << s.candidates
        << ",\n    \"covered_fragments\": " << s.covered_fragments
        << ",\n    \"depth_rejected\": " << s.depth_rejected
        << ",\n    \"keyed_rejected\": " << s.keyed_rejected
        << ",\n    \"texture_samples\": " << s.texture_samples
        << ",\n    \"color_writes\": " << s.color_writes
        << ",\n    \"depth_writes\": " << s.depth_writes;
    if (options.mode == RenderMode::HardwareBlocks) {
        out << ",\n    \"hardware_slice_cycles\": " << s.hardware_cycles
            << ",\n    \"hardware_slice_stalls\": " << s.hardware_stalls
            << ",\n    \"hardware_slice_setup_cycles\": " << s.hardware_setup_cycles
            << ",\n    \"hardware_slice_forwarded_reads\": " << s.hardware_forwarded_reads
            << ",\n    \"hardware_perspective_cycles\": " << s.hardware_perspective_cycles
            << ",\n    \"hardware_texture_cycles\": " << s.hardware_texture_cycles
            << ",\n    \"hardware_texture_hits\": " << s.hardware_texture_hits
            << ",\n    \"hardware_texture_misses\": " << s.hardware_texture_misses
            << ",\n    \"hardware_texture_refill_words\": " << s.hardware_texture_refill_words;
    }
    out << "\n  }";
    if constexpr (std::is_same_v<Engine, hw::SystemModel>) {
        report_system(out, renderer);
        if (throughput) {
            out << ",\n  \"fragment_throughput\": {\n    \"scope\": \"all observed system core edges\","
                << "\n    \"measurements\": ";
            throughput->write_json(out);
            const auto pass_cycles = renderer.system_statistics().last_pass_cycles;
            out << ",\n    \"last_pass_color_writes_per_core_cycle\": "
                << (pass_cycles != 0 ? static_cast<double>(s.color_writes) / pass_cycles : 0.0)
                << "\n  }";
        }
    }
    out << ",\n  \"color_fnv1a64\": \"" << std::hex << buffer_hash(renderer.front_color())
        << "\",\n  \"depth_fnv1a64\": \"" << buffer_hash(renderer.depth()) << "\"\n}\n" << std::dec;
}

#ifdef NANO_GPU_WITH_SDL
const std::vector<std::uint16_t>& visible_color(const Renderer& renderer) {
    return renderer.front_color();
}

const std::vector<std::uint16_t>& visible_color(const hw::SystemModel& renderer) {
    return renderer.display_color();
}

struct SdlSession {
    SdlSession() {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
    }
    ~SdlSession() { SDL_Quit(); }
};

class Presenter {
public:
    Presenter(bool finite, const Options& options) {
        const std::string driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "";
        if (!finite && (driver == "dummy" || driver == "offscreen"))
            throw std::runtime_error("no visible video backend; use --headless or a finite --frames run");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        const auto title = std::string("SRR | ") + scene_name(options.scene) + " | " +
                           render_mode_name(options) + " | Space: pause | S: save | Esc: quit";
        window_.reset(SDL_CreateWindow(title.c_str(),
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
    template<class Engine>
    bool events(bool& paused, const Engine& renderer) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) return false;
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) return false;
                if (event.key.keysym.sym == SDLK_SPACE) paused = !paused;
                if (event.key.keysym.sym == SDLK_s) write_ppm("build/screenshot.ppm", visible_color(renderer));
            }
        }
        return true;
    }
    template<class Engine>
    void show(const Engine& renderer) {
        if (SDL_UpdateTexture(texture_.get(), nullptr, visible_color(renderer).data(), screen_width * 2) != 0 ||
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

template<class Engine>
int run_demo(const Options& options, Engine& renderer) {
#ifdef NANO_GPU_WITH_SDL
    std::unique_ptr<Presenter> presenter;
    if (!options.headless) presenter = std::make_unique<Presenter>(options.frames != 0, options);
#else
    if (!options.headless) throw std::invalid_argument("this build is headless; add --headless or build the SDL target with make");
#endif
    hw::ThroughputMonitor throughput;
    std::ofstream trace_file;
    std::unique_ptr<hw::FragmentCsvWriter> trace_writer;
    if constexpr (std::is_same_v<Engine, hw::SystemModel>) {
        if (!options.trace_path.empty()) {
            trace_file = output_file(options.trace_path);
            trace_writer = std::make_unique<hw::FragmentCsvWriter>(trace_file);
        }
        renderer.set_cycle_observer([&](std::uint64_t core_cycle, const hw::FragmentCycle& event) {
            throughput.observe(event, core_cycle);
            if (trace_writer) trace_writer->write(event, core_cycle);
        });
    }
    std::array<Renderer, 3> references;
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
        if (options.verify) verify_frame(pass, renderer, options.mode, references);
        const auto verified = Clock::now();
        renderer.queue_present(pass.target);
        renderer.vertical_blank();
#ifdef NANO_GPU_WITH_SDL
        if (presenter) presenter->show(renderer);
#endif
        const auto presented = Clock::now();
        timing.setup_ms += std::chrono::duration<double, std::milli>(prepared - start).count();
        timing.render_ms += std::chrono::duration<double, std::milli>(rendered - prepared).count();
        timing.verify_ms += std::chrono::duration<double, std::milli>(verified - rendered).count();
        timing.present_ms += std::chrono::duration<double, std::milli>(presented - verified).count();
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
    if constexpr (std::is_same_v<Engine, hw::SystemModel>) {
        renderer.set_cycle_observer({});
        throughput.require_drained();
        if (trace_writer) trace_file.close();
    }
    if (timing.frames == 0) return 0;
    if (!options.output.empty()) write_ppm(options.output, renderer.front_color());
    if (!options.dump_prefix.empty()) write_dumps(options.dump_prefix, renderer, options, last_time);
    if (!options.stats_path.empty()) {
        auto out = output_file(options.stats_path);
        report(out, renderer, options, timing, options.system ? &throughput : nullptr);
        out.close();
    }
    report(std::cout, renderer, options, timing, options.system ? &throughput : nullptr);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse(argc, argv);
        if (options.help) { usage(); return 0; }
        if (options.hardware_profile) { show_hardware_profile(options); return 0; }
        if (options.system) {
            hw::SystemModel renderer(options.system_config);
            return run_demo(options, renderer);
        }
        Renderer renderer;
        return run_demo(options, renderer);
    } catch (const std::exception& error) {
        std::cerr << "nano-gpu-ref: " << error.what() << '\n';
        return 1;
    }
}
