# Clock-by-clock throughput verification

The throughput harness observes fragment acceptance and retirement on every
modeled edge. It checks token conservation and reports cadence, latency, stage
occupancy and attachment writes separately. These observers do not create GPU
queues, registers, memory ports or clock cycles.

`ThroughputMonitor` and `FragmentCsvWriter` are defined in
[`throughput.hpp`](../../reference/include/nano_gpu/hw/throughput.hpp).
The event contract is in
[`tile_pipeline.hpp`](../../reference/include/nano_gpu/hw/tile_pipeline.hpp).
Tests are in
[`throughput_tests.cpp`](../../reference/tests/throughput_tests.cpp), and the
controlled-workload executable is
[`throughput_bench.cpp`](../../reference/tests/throughput_bench.cpp).

## Build and run

No SDL installation is needed for these executables:

```sh
make -j4 throughput
make throughput-test
./build/throughput-bench --output build/throughput.json
./build/throughput-bench --workload flat --output build/flat-throughput.json --trace build/flat-trace.csv
./build/throughput-bench --workload textured --stalls --output build/textured-stalls.json
```

In Windows MSYS2 UCRT64, append `.exe` when launching the executable, for example
`./build/throughput-bench.exe`. The make targets select the extension automatically.
Use `--help` to list the options. Missing parent output directories are created.
`--trace` requires one named workload; the CSV streams directly to disk rather
than retaining an unbounded history in simulated GPU storage or host memory.
JSON and CSV output paths must differ.

Each workload draws one dense 32x32 resident tile. Tile initialization happens
before observation. The measured window includes command capture, setup,
candidate generation, fragment work and final drain. It excludes CPU setup,
binning, host delivery and tile/SDRAM DMA. `--stalls` injects a deterministic
common clock-enable pattern; those disabled edges remain in elapsed time.
The available workloads are:

| Workload | Purpose |
| --- | --- |
| `flat` | Dense untextured rectangle with passing D24 test and both writes enabled |
| `gouraud` | Dense Gouraud triangle, including edge initialization and interpolation |
| `textured` | Perspective-correct Gouraud-modulated triangle, with cold miss and hits |
| `sprite` | Integer-coordinate sprite with passing depth and multiple cache lines |
| `depth-reject` | Dense rectangle rejected by LESS equality; no attachment writes |
| `key-reject` | Dense sprite rejected by raw RGB565 key; no attachment writes |

`all`, the default, runs all six and emits one JSON report. Each workload has its
own observation window and starts from a newly cleared tile and cold cache.

## What one pixel per cycle means

The running-path target is one eligible fragment accepted and one earlier
passing fragment retired on each consecutive clock after pipeline filling,
under stated conditions such as cache hits and available output ports. Pipeline
latency may exceed one cycle while acceptance and retirement remain consecutive.

The report separates:

- `accepted`: covered samples entering the fragment path. Uncovered candidates
  do not become fragment tokens.
- `retired`: all completed tokens, including commits, depth rejection, key
  rejection and cancellation.
- `committed`: passing fragment retirements. Color/depth write enables may still
  suppress one or both attachment writes.
- `color_writes` and `depth_writes`: enabled attachment writes. Both can describe
  the same fragment and must not be added as two rendered pixels.
- `max_consecutive_accepts`, `max_consecutive_retirements` and
  `max_consecutive_commits`: the longest runs on consecutive timestamps.
- Interval histograms: distances between successive acceptance, retirement or
  commit timestamps. A sustained one-per-cycle stream has intervals of one.
- Latency histogram: retirement timestamp minus acceptance timestamp, including
  intervening disabled edges and, with common-clock observations, system waits.
- `max_inflight` and `occupied_cycles`: retained fragment contexts and cycles
  with a context present before or after the edge.
- `stage_cycles`: pre-edge stage occupancy, including setup and draining.

Histograms use fixed bins for 0 through 256 cycles and an `over_256` overflow
counter. The scoreboard has 64 fixed entries, independent of runtime length;
this is a host verification limit, not the selected GPU's context capacity.

`observed_cycles` counts callbacks. `elapsed_cycles` spans the first through last
observer timestamps. They match when every clock is observed. A caller that
provides sparse common-clock timestamps will see larger elapsed time; occupancy
and stage counts then describe only the observed edges. Tile-local counters
restart on reset, so the default monitor assigns monotonic observation time;
a full-system observer supplies the common core-clock timestamp instead.

`accepts_per_accept_span_cycle` and `commits_per_commit_span_cycle` exclude
leading/trailing setup. They help quantify sustained cadence, but a very short
stream can make these ratios misleading. Check interval histograms and the
length of consecutive runs. `color_writes_per_elapsed_cycle` includes all clocks
in the measured window. The system CLI additionally reports color writes per
complete pass cycle, including fetching and DMA. Color writes include overdraw;
scanout pixels and repeated display frames are not newly rendered fragments.

## Regression coverage and observed baseline

The tests require the dense flat stream to accept and retire all 1024 tokens,
with 1024 consecutive acceptances and commits, 1023 overlapping accept/retire
edges, and one cycle of fragment latency. Capture and drain make its whole
observation window 1026 cycles. The Gouraud workload must retain the same
running cadence after its extra setup work.

Other tests cover deterministic stalls, depth/key rejection, depth-only writes,
ordered same-address depth dependencies, pending/held cancellation on reset,
and stale-event prevention on idle/transfer edges. Scoreboard fault injection
checks missing/duplicate retirements, changed token identity, disabled-edge
activity, incorrect in-flight counts, reused timestamps and illegal writes on
rejection. The monitor requires all accepted tokens to drain at the end of each
completed workload.

The current standalone serial baseline produced the following unstalled results:

| Workload | Observed cycles | Accepted | Longest acceptance run | Longest commit run |
| --- | ---: | ---: | ---: | ---: |
| Flat | 1026 | 1024 | 1024 | 1024 |
| Gouraud | 1050 | 1024 | 1024 | 1024 |
| Textured | 221230 | 1024 | 1 | 1 |
| Sprite | 19714 | 1024 | 1 | 1 |
| Depth rejection | 1026 | 1024 | 1024 | 0 |
| Key rejection | 18690 | 1024 | 1 | 0 |

These textured timings are reported baselines, not limits that future
optimizations must preserve. Functional semantics, token conservation, legal
port use and registered handoffs must remain correct as schedules change.
The flat/Gouraud results establish local running-path cadence under controlled
conditions. They do not establish sustained full-system fill rate, FPGA fit or
achieved clock frequency. See the [system model](system-model.md) and
[storage mapping](storage-map.md) for shared-memory timing and physical mapping.

## CSV fields

Each record contains the observer timestamp, tile-local cycle, stage, enable,
candidate/coverage flags, separate accepted and retired token IDs and primitive
IDs, pixel coordinates, tile address, textured flag, retirement reason,
attachment-write flags and in-flight counts before/after the edge. An edge can
retire one token and accept a different token; separate IDs preserve that
relationship. A zero ID means there was no corresponding event on the edge.

Reset cancellation uses the original accepted identity. Token IDs remain
monotonic across resets of a tile object even though its local cycle counter
restarts. A consumer must never infer retirement from a changed framebuffer or
reuse a stale acceptance flag from an earlier edge.
