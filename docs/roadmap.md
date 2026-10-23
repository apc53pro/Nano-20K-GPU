# GPU architecture and feature roadmap

Status: development sequence, 2026-10-23. The
[M0 specification](spec/README.md) is delivered and tiled rendering is selected;
the first [canonical software reference](software-reference.md), CPU setup
library, conformance suite, and demo scenes are implemented. M0 still needs
independent geometric error analysis and command-trace capture. Hardware
milestones below remain future work.
The [clocked rendering block model](hardware/block-interfaces.md) now covers
the canonical SRR's complete initial drawing feature set, including perspective
textures, shading, keyed sprites, and ordered copies, with matching color/depth
output. It uses verified Gowin multiplier and BSRAM configurations and an exact
serial perspective divider; throughput optimization remains future work.
This is software architecture work; hardware synthesis and board measurements
remain outstanding.
The [system simulation](hardware/system-model.md) now connects rendering to a
timed host link, bounded command queues, shared SDRAM, and independent scanout.
Its clock/timing assumptions are configurable; measured physical feasibility
and a faster rendering pipeline remain M1 work.
Use measured exit criteria to advance. Calendar estimates should follow the
initial feasibility work, once the toolchain and hardware constraints are known.

The [requirements baseline](requirements.md) defines the original nine goals.
The [architecture notes](architecture-notes.md) provide the memory and bandwidth
estimates. Additional features discussed during planning remain candidates;
including them here does not make them mandatory for the first release.

## Architecture direction

Start with a fixed-function, fixed-point GPU. The RP2350 handles transforms,
clipping, projection, triangle setup, and lighting. The FPGA handles coverage,
interpolation, perspective-correct sampling, shading, depth tests, 2D drawing,
and presentation. GPU triangle setup is a later option.

Keep two full-resolution RGB565 color buffers and one full-resolution depth
buffer in SDRAM. Start the design evaluation with 24 meaningful depth bits in
32-bit storage. Use on-chip memory for scanout buffering, commands, texture
caching, and a color/depth working set.

Tiled rendering is confirmed. M0 specifies 32×32 logical tiles, CPU binning, and
prepared primitive records stored once with ordered tile lists referencing them.
Measure physical banking/cache choices, CPU binning, references, and record
fetches; avoiding repeated records does not eliminate binning traffic.

Scanout remains independent of rendering progress. It receives bounded memory
service and continues showing the front buffer if a new frame is not ready.
The host interface starts at 8 bits with flow control; drawing semantics should
remain usable if a wider physical transport becomes necessary.

## Milestones

| Milestone | Architecture and features | Exit gate |
| --- | --- | --- |
| M0 — Contracts and reference | Numeric formats, coverage/depth rules, memory layouts, command semantics, CPU setup library, software reference, benchmark scenes. | Deterministic rendering rules, numeric tolerances, and validation inputs are documented. |
| M1 — Feasibility and physical architecture | SDRAM/video/host-link prototypes, reciprocal pipeline experiment, tile banking/cache evaluation, resource and bandwidth budgets. | Measured feasibility report selects the physical working set and packed submission ABI consistent with M0. |
| M2 — System backbone | SDRAM arbitration, 640×480 scanout, two color buffers, 8-bit transport, uploads, clears, rectangle fills, fences, swaps, counters. | Stable approximately 60 Hz video and tear-free presentation under concurrent host and memory traffic. |
| M3 — Opaque 3D | Selected color/depth working set, CPU-prepared triangles, subpixel coverage, scissor clipping, flat color, at least 24-bit depth. | Coverage and depth agree with the reference; dependency hazards and cache writeback are correct. |
| M4 — Textures and shading | Texture cache, perspective-correct nearest RGB565 sampling, Gouraud shading, texture replacement/modulation, ordered texture updates. | Textures meet numeric tolerances; misses and backpressure preserve correctness; pipeline throughput is measured. |
| M5 — First functional release | Complete baseline 2D operations, clipped/color-key sprites, 2D/3D ordering, RP2350 driver and demos, capability reporting. | GPU-01 through GPU-08 pass their acceptance criteria; GPU-09 has published measurements and a clear account of any gap to the target. |
| M6 — Performance and inexpensive additions | Tune memory/host paths; evaluate indexed textures, dithering, sprite scaling/flipping, font-atlas helpers, and additional addressing modes. | Each admitted addition passes validation with measured area, timing, and throughput impact. |
| M7 — Conditional extensions | Evaluate fog, stencil, blending, and eventual GPU triangle setup against the remaining budget. | Each extension has demonstrated headroom and preserves the validated baseline behavior. |

## M0 — Define behavior before interfaces become expensive to change

Specify subpixel coordinates, edge inclusion, winding, degenerate handling,
viewport/scissor rules, fixed-point ranges, rounding, and overflow behavior.
Define projected depth, clear/comparison semantics, UV reconstruction, texture
addressing, and image comparison tolerances. Reserve guard bits for interpolation.

Define command ordering, resource ownership, upload visibility, completion fences,
and vertical-blank presentation. Sketch a versioned packet format, status/error
registers, and capability queries. Keep triangle/bin packet details provisional
until M1 selects physical implementation and final packing. The logical tiled
submission model is already specified in M0.

Build a software reference renderer and a CPU triangle-setup library. The
reference should support both higher-precision expected results and the chosen
fixed-point behavior, allowing geometric errors to be distinguished from expected
quantization. Define tests and scenes now; add features to the reference as they
enter development. The original functional requirements remain unchanged.

**Exit:** checked reference images and command examples exist; the numeric error
budget is explicit; benchmark workloads and the metrics to collect are defined.

## M1 — Resolve the highest-risk architecture decisions

Prototype SDRAM initialization, refresh, bursts, and competing clients. Bring up
640×480 timing and RGB565 scanout sufficiently to measure usable memory bandwidth
and worst observed service latency with display traffic active. Include FIFO
underflow detection. Record the measurement conditions and required safety margin.

Prototype the RP2350 PIO/DMA link with flow control. Measure useful upload and
command throughput, including pauses, rather than treating bus clock as payload
bandwidth. Check board schematics and electrical timing before assigning pins.

Prototype and synthesize the reciprocal/multiplier path at candidate operand
widths. Measure precision, LUT/FF/BSRAM/DSP cost, timing, and initiation interval.
Plan the intended shading arithmetic alongside it so perspective correction does
not consume resources already needed elsewhere.

Evaluate the selected tiled renderer using representative traces and measured
rates. With 32×32 logical tiles, evaluate resident-tile count, physical banking/
ports, optional internal subtiles, CPU binning time, bin storage, command volume,
and full-buffer depth load/store behavior. Validate the M0 ordering and coherence
rules for 2D operations, blits, and uploads against that implementation.

**Exit:** write an architecture decision record selecting the physical color/depth
working set and packed ABI for the M0 submission model. Publish subsystem/combined
resource estimates, candidate clocks, memory-service budgets, and a host traffic
budget. Choose representative sustained rendering targets from the evidence;
display refresh remains approximately 60 Hz regardless of rendering frame rate.
If fit is uncertain, resolve the conflicting resource demands before adding
optional features or freezing the renderer interface.

## M2 — Integrate memory, display, control, and presentation

Implement the versioned 8-bit command transport, FIFO backpressure, reset,
status/errors, ordered uploads, clears, and rectangle fills. Integrate SDRAM
arbitration and burst-fed scanout, with defined service guarantees for display
and forward progress for other clients. Build the corresponding RP2350 driver.

Implement front/back ownership and vertical-blank swaps. Distinguish accepted
commands, completed rendering, and completed presentation. Fences and presentation
must account for pending memory writes; extend them to cache writeback as caches
are introduced. With only two buffers, block new rendering when no back buffer
is available.

Add cycle, memory-stall, FIFO, and scanout-underflow instrumentation now. Define
consistent counter snapshots/readback and expand the counters with later stages.

**Exit:** uploads and draws survive backpressure and reset; swaps remain tear-free
under competing traffic; scanout reports no underflows over a documented stress
run. Record the tested duration, traffic, clocks, and memory-controller settings.

## M3 — Establish correct opaque 3D rendering

Implement the selected color/depth working set and its refill/writeback path.
Consume CPU-prepared triangle edges and attribute planes. Implement coverage,
subpixel positioning, scissor clipping, flat color, depth interpolation, and
at least 24 meaningful stored depth bits with conditional comparison/update.

Resolve same-pixel dependencies with forwarding or interlocks. Preserve primitive
order where observable, including equal-depth cases. Validate depth clears,
partial draws, working-set changes, and later passes reusing saved depth.

**Exit:** coverage and depth match the defined reference for shared edges,
degenerate/clipped triangles, overlapping geometry, and back-to-back updates to
the same pixel. Demonstrate a rotating opaque scene driven by CPU triangle setup
without CPU per-pixel rendering. Record integrated area, timing, and fill rate.

## M4 — Add perspective-correct textures and shading

Integrate `u/w`, `v/w`, and `1/w` interpolation, UV reconstruction, nearest RGB565
sampling, and the texture cache. Add Gouraud color, texture replacement, and
modulation by flat/interpolated color. Define initial texture limits and address
modes using the M0/M1 numeric and resource results.

Ensure cache misses, evictions, and downstream stalls preserve attributes and
fragment ordering. Define invalidation/visibility for texture uploads. Keep early
depth rejection separate from the final depth/color commit so future transparent
texel rejection can be supported correctly.

**Exit:** a strongly foreshortened checkerboard and additional texture cases meet
the reference tolerances. Exercise cache misses, texture updates, coordinate
boundaries, and shading saturation. Measure covered-fragment initiation interval
with hits and committed throughput with realistic misses and active scanout.

## M5 — Complete and qualify the first functional GPU

Finish pixels, lines, filled rectangles, RGB565 blits, and clipped/color-key
sprites. Specify blit overlap behavior. Share the sampler and memory path where
practical, with explicit coherence between direct copies and cached rendering.
Transparent texels update neither color nor depth. Verify 2D overlays with depth
disabled and mixed 2D/3D command ordering.

Complete the RP2350 driver, capability/error handling, examples, and documented
packet compatibility. Demonstrate native 640×480 textured 3D with a 2D interface,
24-bit depth, shading, and double-buffered presentation.

**Exit:** functional requirements GPU-01 through GPU-08 pass, using the selected
and documented shading/2D feature details. Publish GPU-09 results separately:
pipeline initiation interval, hit-case committed fill rate, sustained scene
throughput, and limiting factors. Do not represent a pipeline target as an
arbitrary-scene guarantee. Any unmet performance preference remains visible.

## M6 — Optimize the baseline and evaluate low-cost additions

Use the counters to identify bottlenecks before tuning. Evaluate burst lengths,
working-set/cache sizes, banking, write combining, arbitration, command packing,
and CPU setup/binning. Revisit bus width only if measured host throughput is a
limiter and the boards have suitable available pins. Wider transport does not
increase SDRAM bandwidth.

Add optional features individually, in this proposed order:

| Priority | Candidate | Dependency and acceptance focus |
| --- | --- | --- |
| 1 | 4-bit/8-bit indexed textures and transparent index | Reuse the sampler; validate palette updates, packing, cache behavior, and depth/color rejection. Measure storage/upload savings and actual memory traffic. |
| 2 | Ordered RGB565 dithering | Retain higher-precision intermediate color; compare gradients and check clipping/rounding. Adds no external framebuffer allocation. |
| 3 | Nearest sprite scaling and flipping | Reuse coordinate generation; verify clipping, addressing, and color-key behavior across scale/flip combinations. |
| 4 | Font-atlas helpers; optional 1-bit glyph expansion | Reuse sprite/blit operations before adding a dedicated glyph path. Measure host overhead and font storage. |
| 5 | Additional repeat/clamp/mirror modes | Prefer simple power-of-two addressing initially; validate boundaries and avoid expensive arbitrary-size wrapping without evidence it is needed. |

**Exit:** publish before/after resource, timing, quality, bandwidth, and frame-time
measurements for each retained feature. Re-run the affected reference cases and
baseline stress scenes. The release may include only the candidates that fit.

## M7 — Admit extensions only against measured remaining capacity

| Candidate | Main design question |
| --- | --- |
| Simple fog | Can interpolation and mixing use the remaining arithmetic budget at the target initiation rate? A CPU-supplied fog factor avoids committing to an eye-distance reconstruction unit. |
| D24S8 stencil | The spare bits in 32-bit depth storage avoid a larger allocation, but comparison/update rules, masks, hazards, and multipass traffic still need implementation and measurement. |
| Constant-alpha/additive blending | Can tile-local destination reads and color arithmetic meet port bandwidth, ordering, and timing needs, including tile load/store traffic? |
| GPU triangle setup | Is CPU setup or setup-record traffic the measured bottleneck, and is there room for setup arithmetic? Preserve the CPU-setup path; moving setup does not automatically move transforms, clipping, or lighting. |

Bilinear filtering, mipmaps, multisample antialiasing, per-pixel lighting, and
programmable shaders remain deferred research items. Their inclusion needs a
separate cost/benefit decision after the baseline is measured.

## Validation and release evidence

Synthesize and check integrated resource use and timing at every hardware
milestone. Record LUTs, flip-flops, physical BSRAM allocation/ports, DSP use,
clock resources, and achieved clocks. Isolated block results do not establish
complete-system fit. Verify clock-domain crossings and reset sequencing as
interfaces are integrated.

Maintain benchmark scenes for large fills, many small triangles, high overdraw,
foreshortened textures, depth precision/equality, and mixed UI/3D with uploads.
Report CPU setup/binning cost, command bandwidth, triangle rate, covered and
committed fragments, texture misses, memory stalls, frame times, and scanout
underflows. Every performance claim must name its scene, mode, clocks, and cache
conditions. Decide tolerances and sustained-rate goals before claiming success.

M0 reference work and independent M1 feasibility experiments can proceed in
parallel. M1 physical architecture work precedes the stable packed drawing ABI;
M2 through M5 then build the validated baseline. M6 and M7 extend that baseline
without removing native resolution, perspective correction, or depth precision
to conceal a resource or performance shortfall.
