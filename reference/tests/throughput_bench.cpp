#include "throughput_workloads.hpp"

#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

using namespace nano_gpu;
using namespace nano_gpu::hw;

namespace {
struct Options { std::string output, trace, workload = "all"; bool stalls = false; };
bool same_output_path(const std::string& first, const std::string& second) {
    namespace fs = std::filesystem;
    std::error_code error;
    // Existing files may be hard links or symlinks to the same output.
    if (fs::equivalent(fs::path(first), fs::path(second), error)) return true;
    const auto normalized = [](const std::string& name) {
        return fs::weakly_canonical(fs::absolute(fs::path(name))).lexically_normal();
    };
    const auto first_path = normalized(first), second_path = normalized(second);
#ifdef _WIN32
    // Native Windows path comparison normally ignores case, including paths
    // whose final component does not exist yet.
    auto first_name = first_path.native(), second_name = second_path.native();
    for (auto& character : first_name) character = static_cast<wchar_t>(std::towlower(character));
    for (auto& character : second_name) character = static_cast<wchar_t>(std::towlower(character));
    return first_name == second_name;
#else
    return first_path == second_path;
#endif
}
Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help") {
            std::cout << "Usage: throughput-bench [--workload all|flat|gouraud|textured|sprite|depth-reject|key-reject]\n"
                << "                        [--output FILE.json] [--trace FILE.csv] [--stalls]\n"
                << "Runs one resident 32x32 tile with standalone block timing, excluding tile DMA.\n"
                << "Trace requires one named workload; events stream directly to the file.\n";
            std::exit(0);
        }
        if (argument == "--stalls") { result.stalls = true; continue; }
        if ((argument == "--output" || argument == "--trace" || argument == "--workload") && i + 1 < argc) {
            auto& value = argument == "--output" ? result.output : argument == "--trace" ? result.trace : result.workload;
            value = argv[++i]; continue;
        }
        throw std::invalid_argument("unknown/incomplete option: " + argument);
    }
    if (!result.trace.empty() && result.workload == "all")
        throw std::invalid_argument("--trace requires a single --workload for an unambiguous clock domain");
    if (!result.output.empty() && !result.trace.empty() && same_output_path(result.output, result.trace))
        throw std::invalid_argument("JSON and CSV paths must differ");
    if (result.workload != "all") (void)throughput_workloads::primitive(result.workload);
    return result;
}
void create_parent(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
}
void workload(std::ostream& out, const std::string& name, const Options& options) {
    OpaqueTile tile;
    tile.clear(0x18e3, depth_max);
    ThroughputMonitor monitor;
    std::ofstream trace;
    std::unique_ptr<FragmentCsvWriter> writer;
    if (!options.trace.empty()) {
        create_parent(options.trace);
        trace.open(options.trace);
        if (!trace) throw std::runtime_error("cannot open trace output: " + options.trace);
        writer = std::make_unique<FragmentCsvWriter>(trace);
    }
    tile.set_cycle_observer([&](const FragmentCycle& event) {
        monitor.observe(event);
        if (writer) writer->write(event, monitor.statistics().last_cycle);
    });
    throughput_workloads::draw(tile, throughput_workloads::primitive(name), options.stalls);
    monitor.require_drained();
    out << "{\"name\":\"" << name << "\",\"clock_domain\":\"resident_tile_enabled_and_disabled_edges\","
        << "\"includes_command_capture_setup_and_drain\":true,\"includes_tile_dma\":false,"
        << "\"fixed_stall_pattern\":" << (options.stalls ? "true" : "false")
        << ",\"expected_covered_fragments\":1024,\"throughput\":";
    monitor.write_json(out);
    const auto& counts = tile.statistics();
    out << ",\"cache_hits\":" << counts.texture_hits << ",\"cache_misses\":" << counts.texture_misses << '}';
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto configuration = options(argc, argv);
        std::ostringstream report;
        report << "{\"schema\":\"N20K-throughput-0.1\",\"mode\":\"blocks\","
            << "\"physical_timing_validated\":false,\"workloads\":[";
        if (configuration.workload == "all") {
            bool comma = false;
            for (const auto name : throughput_workloads::names()) {
                if (comma) report << ',';
                workload(report, name, configuration); comma = true;
            }
        } else workload(report, configuration.workload, configuration);
        report << "]}\n";
        if (!configuration.output.empty()) {
            create_parent(configuration.output);
            std::ofstream output(configuration.output);
            if (!output) throw std::runtime_error("cannot open JSON output: " + configuration.output);
            output << report.str();
            if (!output) throw std::runtime_error("JSON output write failed");
        }
        std::cout << report.str();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "throughput-bench: " << error.what() << '\n';
        return 1;
    }
}
