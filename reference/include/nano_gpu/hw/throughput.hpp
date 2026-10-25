#pragma once

#include "nano_gpu/hw/tile_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <stdexcept>

namespace nano_gpu::hw {

// Host-only verification and reporting. The scoreboard, histograms and CSV
// stream are observers; they add no device registers, queues or memory ports.
struct ThroughputHistogram {
    static constexpr unsigned exact_bins = 257; // 0..256, then overflow.
    std::array<std::uint64_t, exact_bins> bins{};
    std::uint64_t overflow = 0;
    void add(std::uint64_t value) {
        if (value < bins.size()) ++bins[static_cast<unsigned>(value)];
        else ++overflow;
    }
};

struct ThroughputStatistics {
    std::uint64_t cycles = 0, elapsed_cycles = 0;
    std::uint64_t accepted = 0, retired = 0, committed = 0;
    std::uint64_t depth_rejected = 0, key_rejected = 0, cancelled = 0;
    std::uint64_t color_writes = 0, depth_writes = 0;
    std::uint64_t disabled_cycles = 0, occupied_cycles = 0;
    std::uint64_t same_cycle_accept_retire = 0;
    std::uint64_t batch_cancellation_edges = 0;
    unsigned max_cancelled_per_edge = 0;
    unsigned max_inflight = 0;
    std::uint64_t max_consecutive_accepts = 0, max_consecutive_retirements = 0;
    std::uint64_t max_consecutive_commits = 0;
    std::uint64_t first_cycle = 0, last_cycle = 0;
    std::uint64_t first_accept = 0, last_accept = 0;
    std::uint64_t first_retire = 0, last_retire = 0;
    std::uint64_t first_commit = 0, last_commit = 0;
    std::array<std::uint64_t, static_cast<unsigned>(FragmentStage::Reset) + 1> stage_cycles{};
    ThroughputHistogram accept_intervals, retire_intervals, commit_intervals, latencies;
};

class ThroughputMonitor {
public:
    static constexpr unsigned scoreboard_capacity = fragment_trace_capacity;

    // Sequential observer time survives a reset of the tile-local counter.
    void observe(const FragmentCycle& event) {
        observe(event, statistics_.cycles == 0 ? 1 : statistics_.last_cycle + 1);
    }
    // A system scheduler supplies the common-clock timestamp. Gaps are allowed
    // and visible in elapsed_cycles; occupancy/stage counts cover observed edges.
    void observe(const FragmentCycle& event, std::uint64_t timestamp) {
        const auto stage = static_cast<unsigned>(event.stage);
        if (timestamp == 0 || (statistics_.cycles != 0 && timestamp <= statistics_.last_cycle))
            fail("clock timestamps must increase strictly");
        if (stage >= statistics_.stage_cycles.size()) fail("unknown fragment stage");
        if (event.inflight_before != inflight_) fail("incorrect pre-edge in-flight count");
        if (!event.enabled && (event.accepted || event.retired || event.color_write || event.depth_write ||
                               event.cancelled_count != 0))
            fail("disabled edge changed fragment state");
        if (event.accepted && (!event.candidate || !event.covered || event.accepted_fragment.id == 0))
            fail("accepted fragment must identify a covered candidate");
        if (event.retired != (event.retirement != FragmentRetirement::None))
            fail("retirement flag/reason mismatch");
        if ((event.color_write || event.depth_write) &&
            (!event.retired || event.retirement != FragmentRetirement::Commit))
            fail("attachment write without passing retirement");
        if (event.accepted && event.accepted_fragment.id <= last_accepted_id_)
            fail("fragment IDs must increase across resets");
        if (event.cancelled_count != 0) validate_cancellations(event);

        // The existing fast path retires the old fragment before accepting its
        // successor. Support a future combinational accept/retire only when its
        // ID is the new token; it still receives exactly one scoreboard entry.
        const bool new_token_retirement = event.accepted && event.retired &&
            event.accepted_fragment.id == event.retired_fragment.id;
        if (new_token_retirement) accept(event.accepted_fragment, timestamp);
        if (event.retired) retire(event, timestamp);
        if (event.accepted && !new_token_retirement) accept(event.accepted_fragment, timestamp);
        for (unsigned index = 0; index < event.cancelled_count; ++index) {
            FragmentCycle cancellation;
            cancellation.retired_fragment = event.cancelled_fragments[index];
            cancellation.retirement = FragmentRetirement::Cancelled;
            retire(cancellation, timestamp);
        }
        if (event.inflight_after != inflight_) fail("incorrect post-edge in-flight count");

        auto& s = statistics_;
        if (s.cycles == 0) s.first_cycle = timestamp;
        ++s.cycles; s.last_cycle = timestamp; s.elapsed_cycles = timestamp - s.first_cycle + 1;
        ++s.stage_cycles[stage];
        if (!event.enabled) ++s.disabled_cycles;
        if (event.inflight_before != 0 || event.inflight_after != 0) ++s.occupied_cycles;
        s.max_inflight = std::max(s.max_inflight, std::max(event.inflight_before, event.inflight_after));
        if (event.accepted && event.retired) ++s.same_cycle_accept_retire;
        if (event.cancelled_count != 0) {
            ++s.batch_cancellation_edges;
            s.max_cancelled_per_edge = std::max(s.max_cancelled_per_edge, event.cancelled_count);
        }
    }

    const ThroughputStatistics& statistics() const { return statistics_; }
    unsigned inflight() const { return inflight_; }
    void require_drained() const {
        if (inflight_ != 0 || statistics_.accepted != statistics_.retired)
            fail("accepted fragments have not all retired");
    }

    void write_json(std::ostream& out) const {
        const auto& s = statistics_;
        out << "{\"observed_cycles\":" << s.cycles << ",\"elapsed_cycles\":" << s.elapsed_cycles
            << ",\"accepted\":" << s.accepted << ",\"retired\":" << s.retired
            << ",\"committed\":" << s.committed << ",\"depth_rejected\":" << s.depth_rejected
            << ",\"key_rejected\":" << s.key_rejected << ",\"cancelled\":" << s.cancelled
            << ",\"color_writes\":" << s.color_writes << ",\"depth_writes\":" << s.depth_writes
            << ",\"disabled_cycles\":" << s.disabled_cycles << ",\"occupied_cycles\":" << s.occupied_cycles
            << ",\"max_inflight\":" << s.max_inflight
            << ",\"same_cycle_accept_retire\":" << s.same_cycle_accept_retire
            << ",\"batch_cancellation_edges\":" << s.batch_cancellation_edges
            << ",\"max_cancelled_per_edge\":" << s.max_cancelled_per_edge
            << ",\"max_consecutive_accepts\":" << s.max_consecutive_accepts
            << ",\"max_consecutive_retirements\":" << s.max_consecutive_retirements
            << ",\"max_consecutive_commits\":" << s.max_consecutive_commits
            << ",\"first_accept_cycle\":" << s.first_accept << ",\"last_accept_cycle\":" << s.last_accept
            << ",\"first_retire_cycle\":" << s.first_retire << ",\"last_retire_cycle\":" << s.last_retire
            << ",\"first_commit_cycle\":" << s.first_commit << ",\"last_commit_cycle\":" << s.last_commit
            << ",\"accepts_per_observed_cycle\":" << ratio(s.accepted, s.cycles)
            << ",\"color_writes_per_elapsed_cycle\":" << ratio(s.color_writes, s.elapsed_cycles)
            << ",\"accepts_per_accept_span_cycle\":" << span_ratio(s.accepted, s.first_accept, s.last_accept)
            << ",\"commits_per_commit_span_cycle\":" << span_ratio(s.committed, s.first_commit, s.last_commit)
            << ",\"accept_interval_cycles\":";
        histogram_json(out, s.accept_intervals);
        out << ",\"retire_interval_cycles\":"; histogram_json(out, s.retire_intervals);
        out << ",\"commit_interval_cycles\":"; histogram_json(out, s.commit_intervals);
        out << ",\"fragment_latency_cycles\":"; histogram_json(out, s.latencies);
        out << ",\"stage_cycles\":{";
        for (unsigned i = 0; i < s.stage_cycles.size(); ++i) {
            if (i != 0) out << ',';
            out << '"' << fragment_stage_name(static_cast<FragmentStage>(i)) << "\":" << s.stage_cycles[i];
        }
        out << "}}";
        if (!out) throw std::runtime_error("throughput JSON output failed");
    }

private:
    struct Entry { FragmentToken token{}; std::uint64_t accepted_at = 0; };
    std::array<Entry, scoreboard_capacity> scoreboard_{};
    unsigned inflight_ = 0;
    std::uint64_t last_accepted_id_ = 0;
    std::uint64_t accept_run_ = 0, retire_run_ = 0, commit_run_ = 0;
    ThroughputStatistics statistics_;

    [[noreturn]] static void fail(const char* message) { throw std::logic_error(message); }
    static double ratio(std::uint64_t numerator, std::uint64_t denominator) {
        return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
    }
    static double span_ratio(std::uint64_t count, std::uint64_t first, std::uint64_t last) {
        return count == 0 ? 0.0 : ratio(count, last - first + 1);
    }
    static bool same_token(const FragmentToken& a, const FragmentToken& b) {
        return a.id == b.id && a.primitive_id == b.primitive_id && a.x == b.x && a.y == b.y &&
            a.address == b.address && a.textured == b.textured;
    }
    void validate_cancellations(const FragmentCycle& event) const {
        if (event.cancelled_count > scoreboard_capacity || event.cancelled_fragments == nullptr)
            fail("invalid bounded cancellation snapshot");
        if (event.stage != FragmentStage::Reset || !event.enabled || event.accepted || event.retired ||
            event.color_write || event.depth_write || event.inflight_after != 0 ||
            event.cancelled_count != inflight_)
            fail("batch cancellation must atomically drain the pipeline on a reset edge");
        for (unsigned index = 0; index < event.cancelled_count; ++index) {
            const auto& token = event.cancelled_fragments[index];
            const auto found = std::find_if(scoreboard_.begin(), scoreboard_.end(),
                [&](const Entry& entry) { return entry.token.id == token.id && token.id != 0; });
            if (found == scoreboard_.end() || !same_token(found->token, token))
                fail("unknown or changed fragment in cancellation snapshot");
            for (unsigned prior = 0; prior < index; ++prior)
                if (event.cancelled_fragments[prior].id == token.id)
                    fail("duplicate fragment in cancellation snapshot");
        }
    }
    static void cadence(std::uint64_t timestamp, std::uint64_t count, std::uint64_t& first,
                        std::uint64_t& last, std::uint64_t& run, std::uint64_t& maximum,
                        ThroughputHistogram& intervals) {
        if (count == 1) { first = timestamp; run = 1; }
        else {
            intervals.add(timestamp - last);
            run = timestamp == last + 1 ? run + 1 : 1;
        }
        last = timestamp; maximum = std::max(maximum, run);
    }
    void accept(const FragmentToken& token, std::uint64_t timestamp) {
        if (inflight_ == scoreboard_.size()) fail("bounded fragment scoreboard capacity exceeded");
        auto found = std::find_if(scoreboard_.begin(), scoreboard_.end(),
            [](const Entry& entry) { return entry.token.id == 0; });
        *found = {token, timestamp}; ++inflight_; last_accepted_id_ = token.id;
        auto& s = statistics_; ++s.accepted;
        cadence(timestamp, s.accepted, s.first_accept, s.last_accept, accept_run_,
                s.max_consecutive_accepts, s.accept_intervals);
    }
    void retire(const FragmentCycle& event, std::uint64_t timestamp) {
        auto found = std::find_if(scoreboard_.begin(), scoreboard_.end(),
            [&](const Entry& entry) { return entry.token.id == event.retired_fragment.id && entry.token.id != 0; });
        if (found == scoreboard_.end()) fail("unknown or duplicate fragment retirement");
        if (!same_token(found->token, event.retired_fragment)) fail("retired fragment identity changed");
        auto& s = statistics_;
        s.latencies.add(timestamp - found->accepted_at); *found = {}; --inflight_; ++s.retired;
        cadence(timestamp, s.retired, s.first_retire, s.last_retire, retire_run_,
                s.max_consecutive_retirements, s.retire_intervals);
        switch (event.retirement) {
        case FragmentRetirement::Commit:
            ++s.committed;
            cadence(timestamp, s.committed, s.first_commit, s.last_commit, commit_run_,
                    s.max_consecutive_commits, s.commit_intervals);
            break;
        case FragmentRetirement::DepthRejected: ++s.depth_rejected; break;
        case FragmentRetirement::KeyRejected: ++s.key_rejected; break;
        case FragmentRetirement::Cancelled: ++s.cancelled; break;
        default: fail("unknown retirement reason");
        }
        if (event.color_write) ++s.color_writes;
        if (event.depth_write) ++s.depth_writes;
    }
    static void histogram_json(std::ostream& out, const ThroughputHistogram& h) {
        out << '{'; bool comma = false;
        for (unsigned i = 0; i < h.bins.size(); ++i) {
            if (h.bins[i] == 0) continue;
            if (comma) out << ',';
            out << '"' << i << "\":" << h.bins[i]; comma = true;
        }
        if (comma) out << ',';
        out << "\"over_256\":" << h.overflow << '}';
    }
};

class FragmentCsvWriter {
public:
    explicit FragmentCsvWriter(std::ostream& output) : out_(output) {
        out_ << "cycle,local_cycle,stage,enabled,candidate,covered,accepted_id,accepted_primitive,accepted_x,accepted_y,accepted_address,accepted_textured,retired_id,retired_primitive,retired_x,retired_y,retired_address,retired_textured,retirement,color_write,depth_write,inflight_before,inflight_after,cancelled_count,cancelled_ids\n";
        check();
    }
    void write(const FragmentCycle& e, std::uint64_t timestamp) {
        if (e.cancelled_count > fragment_trace_capacity || (e.cancelled_count != 0 && e.cancelled_fragments == nullptr))
            throw std::logic_error("invalid bounded cancellation snapshot for CSV");
        out_ << timestamp << ',' << e.cycle << ',' << fragment_stage_name(e.stage) << ',' << e.enabled
            << ',' << e.candidate << ',' << e.covered << ',' << e.accepted_fragment.id
            << ',' << e.accepted_fragment.primitive_id << ',' << e.accepted_fragment.x
            << ',' << e.accepted_fragment.y << ',' << e.accepted_fragment.address
            << ',' << e.accepted_fragment.textured << ',' << e.retired_fragment.id
            << ',' << e.retired_fragment.primitive_id << ',' << e.retired_fragment.x
            << ',' << e.retired_fragment.y << ',' << e.retired_fragment.address
            << ',' << e.retired_fragment.textured << ',' << fragment_retirement_name(e.retirement)
            << ',' << e.color_write << ',' << e.depth_write << ',' << e.inflight_before
            << ',' << e.inflight_after << ',' << e.cancelled_count << ",\"";
        for (unsigned index = 0; index < e.cancelled_count; ++index) {
            if (index != 0) out_ << ';';
            out_ << e.cancelled_fragments[index].id;
        }
        out_ << "\"\n";
        check();
    }
private:
    std::ostream& out_;
    void check() const { if (!out_) throw std::runtime_error("fragment CSV output failed"); }
};

} // namespace nano_gpu::hw
