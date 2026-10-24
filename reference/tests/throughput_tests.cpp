#include "throughput_workloads.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

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

void fast_path() {
    run("dense flat fragments accept and retire every cycle after pipeline fill", [] {
        OpaqueTile tile; tile.clear(0, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& event) { monitor.observe(event); });
        throughput_workloads::draw(tile, throughput_workloads::primitive("flat"));
        monitor.require_drained();
        const auto& s = monitor.statistics();
        expect(s.accepted == 1024 && s.retired == 1024 && s.committed == 1024,
               "dense tile did not conserve fragment tokens");
        expect(s.color_writes == 1024 && s.depth_writes == 1024,
               "attachment writes must describe one retirement, not two fragments");
        expect(s.cycles == 1026 && s.elapsed_cycles == 1026,
               "flat stream must include capture and final pipeline drain");
        expect(s.max_consecutive_accepts == 1024 && s.max_consecutive_commits == 1024,
               "flat stream failed sustained one-fragment-per-cycle cadence");
        expect(s.accept_intervals.bins[1] == 1023 && s.commit_intervals.bins[1] == 1023,
               "sustained cadence interval counts are wrong");
        expect(s.latencies.bins[1] == 1024 && s.same_cycle_accept_retire == 1023 && s.max_inflight == 1,
               "registered retirement must overlap the following candidate");
        const auto before = s.accepted;
        tile.idle_edge(); tile.read_word_request(0); (void)tile.read_word_result(); tile.idle_edge();
        expect(monitor.statistics().accepted == before && monitor.statistics().retired == before,
               "idle/transfer clocks replayed stale fragment events");
    });
    run("clock-enable stalls preserve fragments while adding real elapsed cycles", [] {
        OpaqueTile normal, stalled; normal.clear(0, depth_max); stalled.clear(0, depth_max);
        ThroughputMonitor a, b;
        normal.set_cycle_observer([&](const FragmentCycle& e) { a.observe(e); });
        stalled.set_cycle_observer([&](const FragmentCycle& e) { b.observe(e); });
        const auto primitive = throughput_workloads::primitive("flat");
        throughput_workloads::draw(normal, primitive);
        throughput_workloads::draw(stalled, primitive, true);
        a.require_drained(); b.require_drained();
        expect(b.statistics().cycles == a.statistics().cycles + b.statistics().disabled_cycles,
               "disabled clocks were omitted or advanced enabled work");
        expect(b.statistics().accepted == 1024 && b.statistics().color_writes == 1024,
               "stall duplicated or lost a fragment");
        normal.set_cycle_observer({}); stalled.set_cycle_observer({});
        const auto na = normal.store(), sb = stalled.store();
        expect(na.color == sb.color && na.depth == sb.depth, "stall changed stored attachments");
    });
    run("dense Gouraud triangles retain one-fragment-per-cycle running cadence", [] {
        OpaqueTile tile; tile.clear(0, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& e) { monitor.observe(e); });
        throughput_workloads::draw(tile, throughput_workloads::primitive("gouraud"));
        monitor.require_drained();
        expect(monitor.statistics().accepted == 1024 && monitor.statistics().max_consecutive_commits == 1024,
               "Gouraud interpolation introduced an unaccounted running-path bubble");
        expect(monitor.statistics().stage_cycles[static_cast<unsigned>(FragmentStage::Setup)] > 0,
               "triangle setup must be visible separately from running cadence");
    });
}

void rejection_and_depth() {
    run("depth rejects retire without color or depth writes", [] {
        OpaqueTile tile; tile.clear(0x18e3, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& e) { monitor.observe(e); });
        throughput_workloads::draw(tile, throughput_workloads::primitive("depth-reject"));
        monitor.require_drained();
        const auto& s = monitor.statistics();
        expect(s.accepted == 1024 && s.depth_rejected == 1024 && s.committed == 0 &&
               s.color_writes == 0 && s.depth_writes == 0, "rejected fragments disappeared or wrote attachments");
    });
    run("raw keyed sprites retire and cannot change either attachment", [] {
        OpaqueTile tile; tile.clear(0x18e3, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& e) { monitor.observe(e); });
        throughput_workloads::draw(tile, throughput_workloads::primitive("key-reject"), true);
        monitor.require_drained();
        const auto& s = monitor.statistics();
        expect(s.accepted == 1024 && s.key_rejected == 1024 && s.committed == 0 &&
               s.color_writes == 0 && s.depth_writes == 0, "key rejection incorrectly committed a fragment");
        tile.set_cycle_observer({}); const auto result = tile.store();
        expect(std::all_of(result.color.begin(), result.color.end(), [](auto v) { return v == 0x18e3; }) &&
               std::all_of(result.depth.begin(), result.depth.end(), [](auto v) { return v == depth_max; }),
               "keyed fragment modified an attachment");
    });
    run("depth-only writes remain single fragment retirements", [] {
        auto rect = std::get<Rect>(throughput_workloads::primitive("flat"));
        rect.state.color_write = false;
        OpaqueTile tile; tile.clear(0x18e3, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& e) { monitor.observe(e); });
        throughput_workloads::draw(tile, rect);
        monitor.require_drained();
        expect(monitor.statistics().committed == 1024 && monitor.statistics().depth_writes == 1024 &&
               monitor.statistics().color_writes == 0, "depth-only path wrote color or doubled fragment counts");
    });
    run("same-address ordered draws preserve depth dependencies and identities", [] {
        auto rect = std::get<Rect>(throughput_workloads::primitive("flat"));
        rect.bounds = {0, 0, 1, 1}; rect.depth = 100;
        OpaqueTile tile; tile.clear(0x18e3, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& e) { monitor.observe(e); });
        throughput_workloads::draw(tile, rect); // Near sample writes.
        rect.depth = 101; throughput_workloads::draw(tile, rect); // Far rejects.
        rect.depth = 100; throughput_workloads::draw(tile, rect); // LESS equality rejects.
        rect.state.depth_func = DepthFunc::LessEqual; throughput_workloads::draw(tile, rect);
        monitor.require_drained();
        expect(monitor.statistics().accepted == 4 && monitor.statistics().depth_rejected == 2 &&
               monitor.statistics().committed == 2, "same-address depth ordering changed across primitive boundaries");
        tile.set_cycle_observer({}); expect(tile.store().depth[0] == 100, "same-address depth was corrupted");
    });
}

void textured_and_reset() {
    run("textured stream exposes serial latency and cache hits/misses", [] {
        OpaqueTile tile; tile.clear(0, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& e) { monitor.observe(e); });
        throughput_workloads::draw(tile, throughput_workloads::primitive("textured"));
        monitor.require_drained();
        const auto& s = monitor.statistics();
        expect(s.accepted == 1024 && s.committed == 1024 && s.max_inflight == 1,
               "textured fragment stream lost or duplicated work");
        expect(s.stage_cycles[static_cast<unsigned>(FragmentStage::PerspectiveU)] != 0 &&
               s.stage_cycles[static_cast<unsigned>(FragmentStage::Texture)] != 0,
               "textured stage occupancy was omitted");
        expect(tile.statistics().texture_hits != 0 && tile.statistics().texture_misses != 0,
               "controlled textured stream did not exercise hits and cold misses");
        // The serialized baseline is intentionally measured, not enshrined as a
        // maximum allowed throughput. Future pipelining may shorten these gaps.
        expect(s.retired == s.color_writes, "textured retirements diverged from passing samples");
    });
    run("reset cancels a pending token once and IDs survive local counter restart", [] {
        OpaqueTile tile; tile.clear(0, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& e) { monitor.observe(e); });
        auto rect = std::get<Rect>(throughput_workloads::primitive("flat")); rect.bounds = {0, 0, 1, 1};
        tile.begin(rect, {0, 0}); tile.tick();
        expect(monitor.inflight() == 1, "test did not retain the pending fragment");
        const auto old_id = tile.last_cycle().accepted_fragment.id;
        tile.reset();
        expect(tile.last_cycle().retirement == FragmentRetirement::Cancelled &&
               tile.last_cycle().retired_fragment.id == old_id, "reset failed to cancel the retained identity");
        throughput_workloads::draw(tile, rect);
        monitor.require_drained();
        expect(monitor.statistics().accepted == 2 && monitor.statistics().cancelled == 1 &&
               monitor.statistics().committed == 1, "reset lost or replayed fragment retirement");
    });
    run("reset cancels a held perspective fragment without attachment commit", [] {
        OpaqueTile tile; tile.clear(0, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& e) { monitor.observe(e); });
        tile.begin(throughput_workloads::primitive("textured"), {0, 0});
        for (unsigned guard = 0; monitor.inflight() == 0; ++guard) {
            expect(guard < 1000, "textured setup never accepted its first sample"); tile.tick();
        }
        for (unsigned i = 0; i < 10; ++i) tile.tick();
        tile.reset(); monitor.require_drained();
        expect(monitor.statistics().accepted == 1 && monitor.statistics().cancelled == 1 &&
               monitor.statistics().color_writes == 0, "held textured fragment committed during cancellation");
    });
}

FragmentCycle accepted() {
    FragmentCycle event;
    event.cycle = 1; event.stage = FragmentStage::Raster;
    event.candidate = event.covered = event.accepted = true;
    event.accepted_fragment = {1, 1, 3, 4, 131, false}; event.inflight_after = 1;
    return event;
}
FragmentCycle retired() {
    FragmentCycle event;
    event.cycle = 2; event.stage = FragmentStage::Commit;
    event.retired = true; event.retired_fragment = accepted().accepted_fragment;
    event.retirement = FragmentRetirement::Commit;
    event.color_write = event.depth_write = true; event.inflight_before = 1;
    return event;
}
void scoreboard_poisoning() {
    run("scoreboard rejects missing/duplicate retirement and changed fragment identity", [] {
        ThroughputMonitor pending; pending.observe(accepted());
        throws([&] { pending.require_drained(); }, "missing retirement went undetected");
        auto changed = retired(); changed.retired_fragment.x = 7;
        throws([&] { pending.observe(changed); }, "changed fragment coordinates went undetected");
        ThroughputMonitor normal; normal.observe(accepted()); normal.observe(retired()); normal.require_drained();
        auto duplicate = retired(); duplicate.inflight_before = 0;
        throws([&] { normal.observe(duplicate); }, "duplicate retirement went undetected");
        ThroughputMonitor unknown;
        auto missing = retired(); missing.inflight_before = 0;
        throws([&] { unknown.observe(missing); }, "unknown retirement went undetected");
    });
    run("scoreboard rejects disabled commits, poisoned counts and invalid timestamps", [] {
        ThroughputMonitor disabled; auto a = accepted(); a.enabled = false;
        throws([&] { disabled.observe(a); }, "disabled fragment acceptance went undetected");
        ThroughputMonitor bad_count; a = accepted(); a.inflight_after = 0;
        throws([&] { bad_count.observe(a); }, "poisoned in-flight count went undetected");
        ThroughputMonitor clock; clock.observe(accepted(), 9);
        throws([&] { clock.observe(retired(), 9); }, "same-edge result timestamp was reused");
        ThroughputMonitor key; key.observe(accepted()); auto k = retired();
        k.retirement = FragmentRetirement::KeyRejected;
        throws([&] { key.observe(k); }, "key rejection attachment write went undetected");
    });
    run("bounded histograms retain overflow and CSV streams complete edge records", [] {
        ThroughputMonitor monitor; monitor.observe(accepted(), 10); monitor.observe(retired(), 500);
        monitor.require_drained();
        expect(monitor.statistics().latencies.overflow == 1 && monitor.statistics().elapsed_cycles == 491 &&
               monitor.statistics().cycles == 2, "timestamp gaps were disguised as observed cycles");
        std::ostringstream json; monitor.write_json(json);
        expect(json.str().find("\"over_256\":1") != std::string::npos, "latency overflow omitted from JSON");
        std::ostringstream csv; FragmentCsvWriter writer(csv); writer.write(accepted(), 10); writer.write(retired(), 500);
        const auto content = csv.str();
        expect(std::count(content.begin(), content.end(), '\n') == 3 &&
               content.find("500,2,commit") != std::string::npos, "CSV did not stream exactly one record per edge");
    });
}
} // namespace

int main() {
    fast_path(); rejection_and_depth(); textured_and_reset(); scoreboard_poisoning();
    std::cout << cases - failures << '/' << cases << " throughput verification cases passed\n";
    return failures == 0 ? 0 : 1;
}
