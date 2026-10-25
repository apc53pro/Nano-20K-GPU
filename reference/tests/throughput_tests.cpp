#include "throughput_workloads.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
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

void warm_cache(OpaqueTile& tile, const Primitive& primitive) {
    throughput_workloads::warm_cache(tile, primitive);
}

void compare_tile(const TileImage& actual, const std::vector<Primitive>& primitives) {
    Renderer canonical;
    Pass pass; pass.primitives = primitives;
    canonical.render(pass, RenderMode::Tiled);
    for (unsigned y = 0; y < tile_size; ++y)
        for (unsigned x = 0; x < tile_size; ++x) {
            const auto local = y * tile_size + x;
            const auto global = y * screen_width + x;
            expect(actual.color[local] == canonical.color(pass.target)[global] &&
                   actual.depth[local] == canonical.depth()[global],
                   "pipelined tile differs from the exact canonical attachment result");
        }
}

void pipeline_dataflow() {
    for (const auto shade : {ShadeMode::TextureReplace, ShadeMode::TextureFlatModulate,
                            ShadeMode::TextureGouraudModulate}) {
        run("warm textured triangles accept and commit1024 consecutive fragments", [shade] {
            auto primitive = throughput_workloads::triangle(true); primitive.state.shade = shade;
            OpaqueTile tile; tile.clear(0, depth_max); warm_cache(tile, primitive);
            ThroughputMonitor monitor;
            tile.set_cycle_observer([&](const FragmentCycle& event) { monitor.observe(event); });
            const auto misses = tile.statistics().texture_misses;
            throughput_workloads::draw(tile, primitive); monitor.require_drained();
            const auto& s = monitor.statistics();
            expect(s.accepted == 1024 && s.committed == 1024 && s.color_writes == 1024 &&
                   s.depth_writes == 1024, "warm textured stream did not conserve passing fragments");
            expect(s.max_consecutive_accepts == 1024 && s.max_consecutive_commits == 1024 &&
                   s.accept_intervals.bins[1] == 1023 && s.commit_intervals.bins[1] == 1023,
                   "warm textured stream failed sustained one-fragment-per-cycle throughput");
            expect(s.max_inflight > 1 && s.max_inflight <= ThroughputMonitor::scoreboard_capacity,
                   "textured pipeline did not overlap bounded fragment contexts");
            expect(tile.statistics().texture_misses == misses,
                   "warm-hit throughput test unexpectedly paid a cache miss");
            tile.set_cycle_observer({}); compare_tile(tile.store(), {primitive});
        });
    }
    run("warm sprites sustain one fragment per cycle without perspective work", [] {
        const auto primitive = throughput_workloads::primitive("sprite");
        OpaqueTile tile; tile.clear(0, depth_max); warm_cache(tile, primitive);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& event) { monitor.observe(event); });
        const auto misses = tile.statistics().texture_misses;
        const auto perspective = tile.statistics().perspective_cycles;
        throughput_workloads::draw(tile, primitive); monitor.require_drained();
        const auto& s = monitor.statistics();
        expect(s.accepted == 1024 && s.committed == 1024 && s.max_consecutive_accepts == 1024 &&
               s.max_consecutive_commits == 1024, "warm sprite failed consecutive fragment throughput");
        expect(tile.statistics().texture_misses == misses && tile.statistics().perspective_cycles == perspective,
               "sprite warm-hit test missed or invoked perspective reconstruction");
        tile.set_cycle_observer({}); compare_tile(tile.store(), {primitive});
    });
    run("cache conflict misses and clock stalls preserve every coordinate and shading factor", [] {
        auto state = throughput_workloads::depth_state();
        state.shade = ShadeMode::TextureGouraudModulate;
        state.texture = throughput_workloads::image(64);
        state.address_u = state.address_v = AddressMode::Repeat;
        const std::array<ScreenVertex, 3> vertices{{
            {0, 0, 0.1L, 1, 0, 0, 255, 19, 77},
            {64, 0, 0.8L, 3, 256, 0, 27, 255, 89},
            {0, 64, 0.4L, 8, 0, 256, 37, 51, 255}}};
        const auto primitive = setup_triangle(vertices, state);
        expect(primitive.has_value(), "conflict workload triangle was culled");
        OpaqueTile normal, stalled; normal.clear(0, depth_max); stalled.clear(0, depth_max);
        ThroughputMonitor a, b;
        normal.set_cycle_observer([&](const FragmentCycle& event) { a.observe(event); });
        stalled.set_cycle_observer([&](const FragmentCycle& event) { b.observe(event); });
        throughput_workloads::draw(normal, *primitive);
        throughput_workloads::draw(stalled, *primitive, true);
        a.require_drained(); b.require_drained();
        expect(a.statistics().accepted == 1024 && b.statistics().accepted == 1024 &&
               a.statistics().committed == b.statistics().committed,
               "miss backpressure lost or duplicated a fragment");
        expect(normal.statistics().texture_misses > 1 && stalled.statistics().texture_misses > 1 &&
               b.statistics().disabled_cycles > 0, "conflict test omitted cache misses or injected stalls");
        normal.set_cycle_observer({}); stalled.set_cycle_observer({});
        const auto image = normal.store(), delayed = stalled.store();
        expect(image.color == delayed.color && image.depth == delayed.depth,
               "cache or clock stalls changed a texture choice or fragment context");
        compare_tile(image, {*primitive});
    });
    run("early depth rejects retire in order beside passing textured fragments", [] {
        const auto primitive = throughput_workloads::triangle(true);
        OpaqueTile tile; tile.clear(0, depth_max); warm_cache(tile, primitive);
        TileImage initial;
        for (unsigned i = 0; i < initial.depth.size(); ++i)
            initial.depth[i] = (i & 1u) == 0 ? depth_max : 0;
        tile.load(initial);
        ThroughputMonitor monitor;
        std::uint64_t last_retired_id = 0;
        tile.set_cycle_observer([&](const FragmentCycle& event) {
            monitor.observe(event);
            if (event.retired) {
                expect(event.retired_fragment.id > last_retired_id,
                       "an early depth rejection bypassed an older textured fragment");
                last_retired_id = event.retired_fragment.id;
            }
        });
        throughput_workloads::draw(tile, primitive, true); monitor.require_drained();
        const auto& s = monitor.statistics();
        expect(s.accepted == 1024 && s.retired == 1024 && s.depth_rejected == 512 &&
               s.committed == 512 && s.color_writes == 512 && s.depth_writes == 512,
               "mixed early-depth stream lost fragments or committed rejected work");
        tile.set_cycle_observer({}); const auto actual = tile.store();
        Renderer canonical;
        Pass pass; pass.primitives.emplace_back(primitive); canonical.render(pass, RenderMode::Tiled);
        for (unsigned y = 0; y < tile_size; ++y)
            for (unsigned x = 0; x < tile_size; ++x) {
                const auto local = y * tile_size + x, global = y * screen_width + x;
                if ((local & 1u) != 0)
                    expect(actual.color[local] == 0 && actual.depth[local] == 0,
                           "depth-rejected sample changed an attachment");
                else expect(actual.color[local] == canonical.color(pass.target)[global] &&
                            actual.depth[local] == canonical.depth()[global],
                            "passing sample lost its position or shading beside a rejected token");
            }
    });
    run("keyed textured depth-only work cannot commit provisional depth before visibility", [] {
        auto primitive = throughput_workloads::triangle(true);
        auto texture = throughput_workloads::image(4);
        for (int y = 0; y < texture->height(); ++y)
            for (int x = 0; x < texture->width(); ++x)
                if (((x + y) & 1) == 0) texture->at(x, y) = 0x4321;
        primitive.state.texture = texture; primitive.state.color_key = 0x4321;
        primitive.state.color_write = false;
        Rect farther{{0, 0, tile_size, tile_size}, throughput_workloads::depth_state(), 0xf00000};
        farther.state.color = {17, 231, 73};
        OpaqueTile tile; tile.clear(0, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& event) { monitor.observe(event); });
        throughput_workloads::draw(tile, primitive, true);
        throughput_workloads::draw(tile, farther, true); monitor.require_drained();
        expect(monitor.statistics().key_rejected > 0 && monitor.statistics().depth_rejected > 0 &&
               tile.statistics().texture_samples == 1024,
               "keyed depth-only workload omitted texture visibility or ordered farther depth rejection");
        tile.set_cycle_observer({}); compare_tile(tile.store(), {primitive, farther});
    });
}

void textured_and_reset() {
    run("cold textured stream overlaps fragments while preserving cache hits and misses", [] {
        OpaqueTile tile; tile.clear(0, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& e) { monitor.observe(e); });
        throughput_workloads::draw(tile, throughput_workloads::primitive("textured"));
        monitor.require_drained();
        const auto& s = monitor.statistics();
        expect(s.accepted == 1024 && s.committed == 1024 && s.max_inflight > 1,
               "textured fragment stream lost or duplicated work");
        expect(tile.statistics().perspective_cycles != 0 && tile.statistics().texture_cycles != 0,
               "pipelined arithmetic or texture work was omitted from block counters");
        expect(tile.statistics().texture_hits != 0 && tile.statistics().texture_misses != 0,
               "controlled textured stream did not exercise hits and cold misses");
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
        const auto& reset = tile.last_cycle();
        expect((reset.retirement == FragmentRetirement::Cancelled && reset.retired_fragment.id == old_id) ||
               (reset.cancelled_count == 1 && reset.cancelled_fragments != nullptr &&
                reset.cancelled_fragments[0].id == old_id), "reset failed to cancel the retained identity");
        throughput_workloads::draw(tile, rect);
        monitor.require_drained();
        expect(monitor.statistics().accepted == 2 && monitor.statistics().cancelled == 1 &&
               monitor.statistics().committed == 1, "reset lost or replayed fragment retirement");
    });
    run("reset cancels all overlapping perspective fragments without attachment commit", [] {
        OpaqueTile tile; tile.clear(0, depth_max);
        ThroughputMonitor monitor;
        tile.set_cycle_observer([&](const FragmentCycle& e) { monitor.observe(e); });
        tile.begin(throughput_workloads::primitive("textured"), {0, 0});
        for (unsigned guard = 0; monitor.inflight() < 8; ++guard) {
            expect(guard < 1000, "textured setup never overlapped eight samples"); tile.tick();
        }
        const auto accepted = monitor.statistics().accepted;
        expect(monitor.statistics().color_writes == 0, "reset test already committed a fragment");
        tile.reset(); monitor.require_drained();
        expect(monitor.statistics().accepted == accepted && monitor.statistics().cancelled == accepted &&
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
void batch_cancellation() {
    run("one reset edge atomically cancels a full bounded fragment pipeline", [] {
        ThroughputMonitor monitor;
        std::array<FragmentToken, fragment_trace_capacity> tokens{};
        for (unsigned index = 0; index < tokens.size(); ++index) {
            auto event = accepted();
            tokens[index] = {index + 1u, 1, static_cast<int>(index % 32), static_cast<int>(index / 32),
                             static_cast<std::uint16_t>(index), true};
            event.accepted_fragment = tokens[index];
            event.inflight_before = index; event.inflight_after = index + 1;
            monitor.observe(event, index + 1);
        }
        FragmentCycle reset;
        reset.cycle = 1; reset.stage = FragmentStage::Reset;
        reset.inflight_before = static_cast<unsigned>(tokens.size());
        reset.cancelled_fragments = tokens.data(); reset.cancelled_count = static_cast<unsigned>(tokens.size());
        monitor.observe(reset, 65); monitor.require_drained();
        const auto& counts = monitor.statistics();
        expect(counts.accepted == 64 && counts.retired == 64 && counts.cancelled == 64 &&
               counts.committed == 0 && counts.color_writes == 0 && counts.depth_writes == 0 &&
               counts.batch_cancellation_edges == 1 && counts.max_cancelled_per_edge == 64 && counts.max_inflight == 64,
               "batch reset lost tokens or counted cancellations as passing pixels");
        expect(counts.cycles == 65 && counts.stage_cycles[static_cast<unsigned>(FragmentStage::Reset)] == 1 &&
               counts.max_consecutive_commits == 0,
               "batch reset expanded one physical edge into many observed clocks or commits");
        for (unsigned latency = 1; latency <= 64; ++latency)
            expect(counts.latencies.bins[latency] == 1, "batch reset omitted a retained fragment's latency");
        std::ostringstream csv; FragmentCsvWriter writer(csv); writer.write(reset, 65);
        const auto text = csv.str();
        expect(std::count(text.begin(), text.end(), '\n') == 2 &&
               text.find("cancelled_count,cancelled_ids") != std::string::npos &&
               text.find(",64,\"1;2;3;4;") != std::string::npos && text.find(";63;64\"") != std::string::npos,
               "CSV did not retain all cancelled IDs on a single reset row");
        auto next = accepted(); next.accepted_fragment.id = 65;
        monitor.observe(next, 66);
        auto completion = retired(); completion.retired_fragment = next.accepted_fragment;
        monitor.observe(completion, 67); monitor.require_drained();
        expect(monitor.statistics().accepted == 65 && monitor.statistics().committed == 1,
               "fresh work could not reuse drained scoreboard slots after batch reset");
    });
    run("batch cancellation rejects missing duplicate changed or partial reset snapshots atomically", [] {
        std::array<FragmentToken, 2> tokens{{accepted().accepted_fragment, accepted().accepted_fragment}};
        tokens[1].id = 2;
        const auto seeded = [&] {
            ThroughputMonitor monitor;
            for (unsigned index = 0; index < tokens.size(); ++index) {
                auto event = accepted(); event.accepted_fragment = tokens[index];
                event.inflight_before = index; event.inflight_after = index + 1;
                monitor.observe(event);
            }
            return monitor;
        };
        FragmentCycle reset;
        reset.stage = FragmentStage::Reset; reset.inflight_before = 2;
        reset.cancelled_fragments = tokens.data(); reset.cancelled_count = 2;
        const auto rejected = [&](const FragmentCycle& invalid) {
            auto monitor = seeded();
            throws([&] { monitor.observe(invalid, 3); }, "invalid batch reset was accepted");
            expect(monitor.inflight() == 2 && monitor.statistics().retired == 0,
                   "failed reset validation partly consumed a cancellation snapshot");
            monitor.observe(reset, 3); monitor.require_drained();
        };
        auto invalid = reset; invalid.cancelled_fragments = nullptr; rejected(invalid);
        invalid = reset; invalid.cancelled_count = fragment_trace_capacity + 1; rejected(invalid);
        invalid = reset; invalid.cancelled_count = 1; rejected(invalid);
        invalid = reset; invalid.enabled = false; rejected(invalid);
        invalid = reset; invalid.stage = FragmentStage::Commit; rejected(invalid);
        invalid = reset; invalid.accepted = true; invalid.candidate = invalid.covered = true;
        invalid.accepted_fragment = tokens[1]; invalid.accepted_fragment.id = 3; rejected(invalid);
        auto changed = tokens; changed[1] = changed[0];
        invalid = reset; invalid.cancelled_fragments = changed.data(); rejected(invalid);
        changed = tokens; changed[1].x += 1; rejected(invalid);
        changed = tokens; changed[1].id = 3; rejected(invalid);
        std::ostringstream csv; FragmentCsvWriter writer(csv);
        invalid = reset; invalid.cancelled_fragments = nullptr;
        throws([&] { writer.write(invalid, 3); }, "CSV dereferenced a missing cancellation snapshot");
    });
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
    fast_path(); rejection_and_depth(); pipeline_dataflow(); textured_and_reset(); scoreboard_poisoning(); batch_cancellation();
    std::cout << cases - failures << '/' << cases << " throughput verification cases passed\n";
    return failures == 0 ? 0 : 1;
}
