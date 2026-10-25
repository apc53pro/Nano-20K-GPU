# Further architecture optimization review

Baseline: `b434df20a15411f17cea24ad860fa413d5c38af1`.

The next priorities are tighter CPU binning, persistent primitive updates and a
texture layout that matches two-dimensional access. Temporary prototypes already
demonstrate useful cycle savings without allocating more native BSRAM or DSPs.
Production rendering behavior remains at the baseline; the experiments and
proposals below require adoption and regression work.

All timings are **modeled GPU submission time**, including uploads and rendering
at 100.8 MHz core/SDRAM and 25.2 MHz host clocks. They exclude RP2350 execution,
boot, PRESENT/vblank and diagnostic scanout observation. The measurements use a
frozen time-zero second frame. They are neither SDL wall-clock FPS nor achieved
FPGA performance. Use `--mode system`; standalone `--mode blocks` does not model
the host, fetched records or SDRAM tile transfers.

## Measured prototypes

| Scene | Current submission | Conservative tile bins | 8x2 texture layout |
| --- | ---: | ---: | ---: |
| Showcase | 32.396 ms | 30.035 ms | 30.214 ms |
| Perspective | 26.321 ms | 24.259 ms | 26.066 ms |
| Overdraw | 43.652 ms | 35.692 ms | 43.265 ms |
| Opaque | 48.284 ms | 21.543 ms | Unchanged; no textures |

The combined binning and texture-layout showcase was also measured: **27.845 ms
submission, 22.941 ms rendering pass**, versus the current 32.396/27.445 ms.
That combined result is a separate verified experiment, rather than a sum of
independent savings. All prototype runs retained the committed RGB565/D24 hashes
and reported zero scanout underflows.

Evidence and reproduction:

- [Current workload profile](../benchmarks/architecture-review-profile.json).
- [Fine-binning report](../benchmarks/fine-binning-review/results.json),
  [experimental patch](../benchmarks/fine-binning-review/prototype.patch), and
  [reproduction instructions](../benchmarks/fine-binning-review/README.md).
- [Texture-layout and cache-policy results](../benchmarks/texture-layout-review.json)
  and [experimental patch](../benchmarks/texture-layout-review.patch).

## 1. Reject provably empty triangle/tile intersections on the CPU

`bin_primitives()` currently uses the clipped bounding box. Many submitted
triangle/tile pairs therefore contain no covered pixels. For each edge, find its
maximum over the intersection rectangle of pixel centers. Reject the tile if
any maximum is negative, or zero for an exclusive edge. This is a conservative
broad-phase test; it does not rasterize pixels on the CPU or move the immutable
attribute anchor.

| Scene | Removed triangle references | Candidate visits avoided | Existing setup cycles avoided |
| --- | ---: | ---: | ---: |
| Showcase | 305 / 633 | 181,737 | 74,009 |
| Perspective | 238 / 413 | 177,104 | 62,231 |
| Overdraw | 989 / 2,401 | 636,236 | 222,173 |
| Opaque | 2,482 / 3,539 | 1,980,011 | 775,997 |

This is the highest-confidence next change: the full-model results above
include its effects on fetching, cache state, scheduling and writeback. FPGA
cost is zero additional BSRAM, DSPs or ports. RP2350 binning time still needs
measurement. The M0 [binning wording](../spec/architecture.md#tile-partition-and-binning)
currently requires all bounding-box tiles and needs clarification to permit
omission of provably empty ones. Update candidate/reference counters and tests;
test shared edges, subpixel positions, clipping, scissors and thin triangles.

## 2. Retain primitive data and upload only changed extents

`HostPassCompiler` retains textures but rebuilds and uploads the whole transient
record/bin/table/descriptor heap. Most showcase UI data and much scene state
remain unchanged between frames.

A comparison of the actual showcase transition from time zero to 1/60 second,
including physical target 1 to 0, found:

| Quantity | Measured bytes/count |
| --- | ---: |
| Current transient heap upload | 121,256 bytes |
| Changed bytes | 720 bytes |
| Aligned words containing changes | 336 words |
| Coalesced WRITE_HEAP packets | 36 packets |
| Complete proposed bus traffic, including replies | 3,028 bytes |

Coalescing includes unchanged gaps where sending them costs less than another
24-byte upload header. At 25.2 MHz, raw wire time falls from 4.931 to 0.120 ms
for this frame pair. The **4.81 ms difference is a traffic bound**, not a measured
complete-frame speedup or a guarantee for arbitrary animation.

Ordered delta WRITE_HEAP requests after the prior fence preserve the current
ABI and resource lease. An exact prior-heap shadow costs about 118.4 KiB of CPU
RAM; explicit dirty tracking with stable record IDs is preferable for the
RP2350 driver. Reset, abort, failed transfers, relocation and heap compaction
must invalidate or transactionally rebuild the retained state. No extra FPGA
BSRAM or DSP is intrinsically needed.

## 3. Store eligible textures in 8x2 blocks

Current 32-byte cache lines contain sixteen horizontally adjacent RGB565
texels. An 8x2 block places two neighboring rows in that same line. The
prototype keeps the existing 64-line direct-mapped cache, its single native
BSRAM, two address multipliers and synchronous ports.

| Scene | Current misses | 8x2 misses |
| --- | ---: | ---: |
| Showcase | 26,900 | 19,908 |
| Perspective | 16,213 | 15,039 |
| Overdraw | 4,486 | 2,645 |

The address mapping uses the existing multiply pipeline with a changed X
offset and row-pair address. Three stages of X metadata grow by one bit each;
layout selection and validation add control logic. Production needs an explicit
descriptor layout/version, matching upload conversion, and linear fallback for
small or otherwise unsupported textures. The experimental patch infers layout
from dimensions and is intentionally insufficient as a production format.

Trace replay also ruled out attractive but weak alternatives for these scenes:
retaining tags across bindings alone reduced showcase misses by only 1.7%;
two/four-way caches brought no useful improvement; unconditional 4x4 layout
increased perspective misses. Do not spend scarce logic on associativity
before testing the access layout.

## 4. Bound per-tile attribute initialization

Triangle setup takes `25 + offset_x + offset_y` cycles per reference. The
`OffsetX`/`OffsetY` states move planes one pixel at a time to reach a tile.
Large offsets repeat hundreds of additions for unchanged gradients.

A binary shift/add schedule can use the existing `plane_current` work registers,
`plane_row` accumulators and adders, with fixed one-bit shifts and no new DSP or
BSRAM. Preserve checked s64 arithmetic and the M0 plane-bound proof. After the
fine-bin filter, the estimated reductions in setup work are about 0.59 ms
showcase, 0.43 ms perspective, 3.96 ms overdraw and 2.54 ms opaque. These are
scheduled-work bounds before memory overlap, not measured frame gains.

An 8x8 hierarchical coverage test can then skip empty subregions within
remaining tiles. It needs edge extrema, skip control and exact plane jumps;
its comparator/addition overhead and LUT/FF cost need a cycle model. Pipelining
the six serialized edge products through the existing II1 multiplier is a
smaller additional opportunity.

## 5. Pipeline logical SDRAM slots before adding physical memory

The steady showcase spends 1,575,156 cycles in background export. Of those,
1,253,347 overlap foreground work; bank-reuse waits expose about **3.14 ms**.
The full 15.63 ms exporter activity is not removable frame time.

Each SDRAM client already has two 1024x16 native banks, but currently uses one
16x32-word logical transaction slot. Two disjoint logical write slots could
prepare the next burst through A ports while the active write streams through
B ports. This requires a second ownership/metadata entry, masks and recovery
logic, but no new BSRAM, DSPs or native ports. Read-response writes must retain
A-port priority if the client also supports reads.

Combining the two 64-byte depth halves into a 128-byte internal request could
reduce tile-write requests from 28,800 to 19,200 per frame. Physical BL8 beats
remain unchanged. The controller's 16-word contract, refresh bound, arbitration
fairness and scanout latency need revalidation. Benefits overlap the double-slot
proposal and should be measured separately and together.

For sparse overlay passes, skip attachment traffic for an empty tile with
LOAD/LOAD operations while still counting its logical visit. CLEAR passes must
persist their new clear values. Packed D24, lazy clear tags and cross-frame
upload/render concurrency require explicit memory-view or resource-lease
changes and are larger projects.

## 6. Compact records and reuse idle reader capacity

All kinds currently occupy 320 bytes. Preserving existing leading fields with
320-byte triangles, 80-byte rectangles/sprites and 88-byte lines would reduce
showcase wire traffic from 124,256 to 42,312 bytes and useful record-fetch
traffic from 494,720 to 276,496 bytes. These are layout calculations; burst
rounding and type/length fetching reduce the realized gain. A versioned packed
record/reference contract and validation are required. Compact-record upload
savings overlap delta-upload savings.

An 11-entry record cache can also fit inside existing reader RAM: reserve the
low 144 x32 words for metadata/readback, including its 129th alignment-guard
word, and use the upper 880 words for eleven 80-word records. Misses fill that
area while capturing decoded fields; hits still require eighty synchronous
reads. Address tags, replacement state and a small FSM consume registers/LUTs,
with no extra BSRAM or DSPs.

LRU replay predicts 56.7% showcase and 69.9% perspective hits, but only 13.7%
overdraw and 10.7% opaque hits. Existing lookahead already hides most fetch
work: showcase exposes about 0.78 ms waiting for the next record. Benchmark the
actual contention savings after fine binning before prioritizing this cache.

## Physical timing and implementation order

Retiming the measured reciprocal stage17-to18 path is a parallel priority.
Quotient selection, guard/sticky reduction and rounding share the critical
11.214 ns path. Splitting selection and rounding across registered stages is
estimated to cost roughly 100–160 FFs, no DSPs or BSRAM, and one extra latency
edge while preserving II1. It requires exact metadata alignment, numerical and
stall verification, synthesis and routing. This is a proposal, not a new Fmax.
The [existing arithmetic qualification](rtl-qualification.md) remains 89.17 MHz.

Implement conservative CPU bins first, then ordered dirty updates, the explicit
8x2 texture format, and bounded seed initialization. Reprofile before choosing
double slots, compact records or a record cache. Keep the native allocation
within the current 39/46 BSRAM and 46/48 multiplier-equivalent budget where
possible, and measure LUT/FF cost rather than treating unused RAM addresses as
free control logic.

A useful engineering target is **at most 25 ms modeled submission time** for
the showcase at the stated clocks, leaving more room for CPU preparation and
timing variation. That is a target for the next implementation, not a predicted
combined result or a physical 30-FPS claim.
