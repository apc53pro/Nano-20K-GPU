#include "nano_gpu/hw/reciprocal.hpp"
#include "uv_oracle.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using nano_gpu::hw::PerspectivePipeline;
using nano_gpu::tests::OracleWide;
using nano_gpu::tests::UvVector;

namespace {
unsigned cases = 0, failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void run(const char* name, Function&& operation) {
    ++cases;
    try { operation(); }
    catch (const std::exception& error) {
        ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

PerspectivePipeline::Output expected(const PerspectivePipeline::Input& input) {
    PerspectivePipeline::Output output;
    output.tag = input.tag; output.valid = input.valid;
    if (!input.valid) return output;
    output.invalid_q = input.q < (std::int64_t{1} << 29) || input.q > (std::int64_t{1} << 37);
    if (output.invalid_q) return output;
    try {
        output.u = nano_gpu::tests::rational_uv(input.u, input.q);
        output.v = nano_gpu::tests::rational_uv(input.v, input.q);
    } catch (const std::overflow_error&) { output.overflow = true; }
    return output;
}

bool same_output(const PerspectivePipeline::Output& a, const PerspectivePipeline::Output& b) {
    return a.u == b.u && a.v == b.v && a.tag == b.tag && a.valid == b.valid &&
        a.overflow == b.overflow && a.invalid_q == b.invalid_q;
}

void check_output(const PerspectivePipeline::Output& actual,
                  const PerspectivePipeline::Output& oracle) {
    expect(actual.valid == oracle.valid, "pipeline result-valid did not match the independent edge schedule");
    if (!oracle.valid) return;
    expect(actual.tag == oracle.tag, "UV context tag was changed or reordered");
    expect(actual.invalid_q == oracle.invalid_q, "q validation differs from the numeric contract");
    if (oracle.invalid_q) return;
    expect(actual.overflow == oracle.overflow, "UV overflow differs from independent rational arithmetic");
    if (oracle.overflow) return;
    if (actual.u != oracle.u || actual.v != oracle.v)
        throw std::runtime_error("reciprocal reconstruction differs from exact RNE: got (" +
            std::to_string(actual.u) + "," + std::to_string(actual.v) + "), expected (" +
            std::to_string(oracle.u) + "," + std::to_string(oracle.v) + ")");
}

struct Scheduled {
    std::uint64_t due = 0;
    PerspectivePipeline::Output result{};
};

void stream(const std::vector<UvVector>& vectors, bool stalls, bool bubbles) {
    PerspectivePipeline pipeline;
    std::deque<Scheduled> pending;
    std::size_t next = 0, retired = 0;
    std::uint64_t enabled_edges = 0, elapsed = 0, last_retirement = 0, longest = 0, run_length = 0;
    while (next < vectors.size() || !pending.empty() || pipeline.busy()) {
        expect(++elapsed <= vectors.size() * 4 + 256, "UV stream did not make bounded progress");
        const bool enabled = !stalls || (elapsed % 5 != 1 && elapsed % 11 != 7);
        PerspectivePipeline::Input input;
        if (enabled && next < vectors.size() && (!bubbles || elapsed % 3 != 1)) {
            const auto& sample = vectors[next];
            input.u = sample.u; input.v = sample.v; input.q = sample.q;
            input.tag = static_cast<std::uint8_t>(next % 64); input.valid = true;
            pending.push_back({enabled_edges + PerspectivePipeline::enabled_edge_latency, expected(input)});
            ++next;
        } else if (!enabled) {
            // Disabled inputs intentionally violate the numeric contract. CE
            // must freeze old registers, not validate/capture these pin values.
            input.u = std::numeric_limits<std::int64_t>::min();
            input.v = std::numeric_limits<std::int64_t>::max();
            input.q = 0; input.valid = true; input.tag = 63;
        }
        const auto old = pipeline.output(); const auto old_busy = pipeline.busy();
        pipeline.tick(input, enabled);
        if (!enabled) {
            expect(same_output(old, pipeline.output()) && pipeline.busy() == old_busy,
                   "CE-low changed a UV register or captured poisoned inputs");
            continue;
        }
        ++enabled_edges;
        const bool due = !pending.empty() && pending.front().due == enabled_edges;
        PerspectivePipeline::Output oracle;
        if (due) oracle = pending.front().result;
        check_output(pipeline.output(), oracle);
        if (due) {
            pending.pop_front(); ++retired;
            run_length = last_retirement + 1 == elapsed ? run_length + 1 : 1;
            longest = std::max(longest, run_length); last_retirement = elapsed;
        }
        expect(pending.empty() || pending.front().due > enabled_edges,
               "a valid UV result was omitted from its registered output edge");
    }
    expect(retired == vectors.size(), "UV stream lost or duplicated a result");
    if (!stalls && !bubbles)
        expect(longest == vectors.size(), "a continuous UV stream failed one-result-per-cycle throughput");
}

std::vector<UvVector> endpoint_vectors() {
    std::vector<UvVector> result;
    constexpr auto minimum_q = std::int64_t{1} << 29;
    constexpr auto maximum_q = std::int64_t{1} << 37;
    for (const auto q : {minimum_q, minimum_q + 1, maximum_q - 1, maximum_q}) {
        for (const auto target : {std::numeric_limits<std::int32_t>::min(),
                std::numeric_limits<std::int32_t>::min() + 1, -1, 0, 1,
                std::numeric_limits<std::int32_t>::max() - 1, std::numeric_limits<std::int32_t>::max()}) {
            const auto attribute = static_cast<std::int64_t>(
                (static_cast<OracleWide>(target) * q) / (OracleWide{1} << 26));
            for (const auto delta : {-8, -4, -1, 0, 1, 4, 8})
                result.push_back({attribute + delta, -attribute - delta, q});
        }
        result.push_back({std::numeric_limits<std::int64_t>::min(), 0, q});
        result.push_back({0, std::numeric_limits<std::int64_t>::max(), q});
        result.push_back({std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min(), q});
    }
    for (const auto q : {std::int64_t{0}, minimum_q - 1, maximum_q + 1,
            std::int64_t{-1}, std::numeric_limits<std::int64_t>::min()})
        result.push_back({1, -1, q});
    return result;
}


} // namespace

int main() {
    run("independent rational vectors retain exact signed rounding and continuous II1", [] {
        stream(nano_gpu::tests::perspective_vectors(), false, false);
    });
    run("UV reconstruction and context order survive clock stalls and input bubbles", [] {
        stream(nano_gpu::tests::perspective_vectors(), true, true);
    });
    run("signed Q16 endpoints, overflow and illegal q report bounded results", [] {
        stream(endpoint_vectors(), false, false); stream(endpoint_vectors(), true, true);
    });
    run("full signed quotient domain and normalization carries retain exact correction", [] {
        const auto vectors = nano_gpu::tests::full_q16_vectors();
        stream(vectors, false, false); stream(vectors, true, true);
    });
    run("reset cancels every occupied UV stage and permits fresh traffic", [] {
        PerspectivePipeline pipeline;
        for (unsigned i = 0; i < PerspectivePipeline::enabled_edge_latency / 2; ++i) {
            PerspectivePipeline::Input input;
            input.u = i + 1; input.v = -static_cast<std::int64_t>(i + 1);
            input.q = std::int64_t{1} << 29; input.tag = static_cast<std::uint8_t>(i); input.valid = true;
            pipeline.tick(input);
        }
        expect(pipeline.busy(), "reset test did not fill the arithmetic pipeline");
        pipeline.reset();
        expect(!pipeline.busy() && !pipeline.output().valid, "reset left an old arithmetic result valid");
        for (unsigned i = 0; i < PerspectivePipeline::enabled_edge_latency + 2; ++i) {
            pipeline.tick({}); expect(!pipeline.output().valid, "cancelled result reappeared after reset");
        }
        PerspectivePipeline::Input fresh;
        fresh.u = -48; fresh.v = 48; fresh.q = std::int64_t{1} << 29;
        fresh.tag = 37; fresh.valid = true;
        pipeline.tick(fresh);
        for (unsigned edge = 1; edge < PerspectivePipeline::enabled_edge_latency; ++edge) {
            expect(!pipeline.output().valid, "fresh result crossed a register boundary too early after reset");
            pipeline.tick({});
        }
        check_output(pipeline.output(), expected(fresh));
        pipeline.tick({}); expect(!pipeline.output().valid, "fresh result was replayed after reset recovery");
    });
    run("out-of-width context tags fail before advancing any arithmetic stage", [] {
        PerspectivePipeline pipeline;
        PerspectivePipeline::Input legal;
        legal.u = 123456789; legal.v = -987654321;
        legal.q = std::int64_t{1} << 32; legal.tag = 63; legal.valid = true;
        pipeline.tick(legal);
        const auto before = pipeline.output();
        auto bad = legal; bad.tag = 64;
        bool rejected = false;
        try { pipeline.tick(bad); } catch (const std::out_of_range&) { rejected = true; }
        expect(rejected && same_output(before, pipeline.output()) && pipeline.busy(),
               "invalid context tag mutated a pending arithmetic stream");
        for (unsigned edge = 1; edge < PerspectivePipeline::enabled_edge_latency; ++edge) {
            expect(!pipeline.output().valid, "rejected tag advanced an older token");
            pipeline.tick({});
        }
        check_output(pipeline.output(), expected(legal));
    });
    std::cout << cases - failures << '/' << cases << " perspective pipeline cases passed\n";
    return failures == 0 ? 0 : 1;
}
