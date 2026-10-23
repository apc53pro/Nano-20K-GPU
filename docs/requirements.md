# GPU requirements baseline

Status: initial baseline, 2026-10-23. Target: RP2350-GP-COMPUTER with a Sipeed
Tang Nano 20K using the GW2AR-LV18QN88C8/I7 FPGA.

The nine requirements below come from the project owner. Where the original
request leaves details open, the proposed choices are labeled explicitly.
Acceptance criteria describe future verification; they are not completed tests.

## Requested characteristics

| ID | Requirement | Initial interpretation and acceptance criterion |
| --- | --- | --- |
| GPU-01 | 640×480, 16-bit color video over HDMI | Native 640×480 RGB565 framebuffer, expanded to RGB888 for TMDS transmission through the board's HDMI connector. Approximately 60 Hz, video-only DVI-compatible signaling is confirmed sufficient. Verify timing, color ramps, and stable output on hardware. |
| GPU-02 | Hardware-accelerated 3D rendering | RP2350 performs transforms, clipping, projection, triangle setup, and tile binning initially. FPGA performs tiled coverage, attribute interpolation, texturing, shading, depth testing, and framebuffer writes. Verify rendering from triangle commands without CPU per-pixel drawing. GPU triangle setup is a later extension if resources permit. |
| GPU-03 | Perspective-correct textures | Interpolate `u/w`, `v/w`, and `1/w`, reconstruct texture coordinates in the GPU, then sample the texture. Verify a strongly foreshortened textured surface against a software reference using the error limits in the M0 numeric specification; affine interpolation alone does not satisfy this requirement. |
| GPU-04 | Double buffering | Two complete color framebuffers. Display the front buffer while rendering to the back buffer. Swap only after rendering and memory writes complete, at vertical blanking. Verify tear-free swaps under rendering load. |
| GPU-05 | At least 24-bit depth | One full-resolution logical depth buffer with at least 24 meaningful stored depth bits, GPU depth comparison, clear, and conditional update. Verify overlapping geometry, equality behavior, and cases distinguishable with 24-bit depth but not 16-bit depth. |
| GPU-06 | Shading options | Provide multiple selectable shading modes. Proposed initial set: flat color, Gouraud vertex color, texture replacement, and texture modulation by flat or interpolated color. Verify each implemented mode against a software reference. |
| GPU-07 | 2D rendering | Provide hardware 2D drawing. Proposed initial operations: pixels, lines, filled rectangles, RGB565 blits, and sprites with color-key transparency and clipping. Verify clipping, image copies, and composition with 3D output. |
| GPU-08 | 8-bit parallel control bus, wider if needed | Command and upload interface with an initial 8-bit data path. Include flow control and readable status; measure usable bandwidth with the RP2350. Reserve a protocol path to wider transfers if pin availability and measurements justify it. |
| GPU-09 | Preferably at least one pixel per cycle | Preserve this as a performance goal. Proposed first measurable target: initiation interval of one covered fragment per core cycle through the opaque textured pipeline when required data is available and there are no hazards. Measure sustained committed pixels/cycle separately, including stalls and misses. Core frequency and workload guarantees remain open. |

## Confirmed display decision

The project owner confirmed approximately 60 Hz, video-only DVI-compatible TMDS
through the HDMI connector. Audio and HDMI-specific data packets are outside the
initial baseline. RGB565 gives 65,536 framebuffer colors; expanding channels for
transmission does not increase framebuffer color precision.

Display refresh and rendering frame rate are separate: scanout must remain stable
at approximately 60 Hz even when rendering takes longer than one refresh period.
In that case the front buffer is displayed again until a completed frame is ready.

## Proposed rendering behavior

Tiled rendering is now confirmed by the project owner. The
[M0 specification](spec/README.md) defines the initial engineering profile:
32×32 logical tiles, CPU binning, ordered per-tile references, and local GPU
color/depth working storage backed by complete SDRAM attachments. Physical
banking, resident-tile count, caches, clocks, and the packed ABI remain M1 work.
The details below are refined by that profile and may be explicitly versioned
if feasibility measurements require changes.

- Use a fixed-function pipeline and fixed-point arithmetic initially. The M0
  profile defines coordinate ranges, rounding, saturation, and tolerances.
- Use a deterministic triangle edge rule, such as the top-left rule, and subpixel
  vertex coordinates so adjacent triangles share edges without cracks or double
  coverage. The GPU performs per-fragment coverage; CPU triangle setup produces
  edge equations, bounds, attribute starting values, and gradients.
- Clip against the near plane on the CPU before perspective division. Reject
  degenerate triangles safely. Use GPU viewport/scissor clipping to bound writes.
- Begin with nearest-neighbor RGB565 textures. M0 specifies power-of-two sizes
  from 1 to 1024, clamp/repeat addressing, and fixed-point UV precision.
  Bilinear filtering and mipmaps are later options.
- Calculate lighting on the RP2350 initially. Flat and Gouraud shading consume
  supplied colors; texture modulation multiplies sampled color by supplied color.
  Programmable shaders and per-pixel lighting are later options.
- Use normalized projected depth (`z/w`, mapped to the depth range), interpolated
  in screen space. Proposed convention: near = 0, far = `0xFFFFFF`, compare LESS,
  and clear = `0xFFFFFF`. An exact far-plane value fails LESS; software/reference
  rendering must use the same convention. Depth test and write enables should be
  independently controllable, allowing 2D overlays to bypass depth.
- Keep at least 24 depth bits through storage and comparison. Intermediate
  interpolation arithmetic needs sufficient guard bits; widening a 16-bit result
  to 24 bits does not meet the precision requirement. Near/far plane selection
  also affects useful geometric precision.
- A 24-bit depth value may occupy a 32-bit SDRAM word. The extra bits do not imply
  stencil support. One depth buffer is sufficient while only one frame is being
  rendered; scanout reads color only.
- Apply 2D and 3D commands in a defined order. Blit overlap semantics and texture
  or framebuffer cache coherence must be specified before those operations ship.
  Alpha blending and fog are later options; color-key transparency is proposed
  for the initial sprite path.

## Proposed host interface behavior

Use the RP2350's PIO and DMA to stream commands and bulk texture/image uploads to
a FIFO-backed interface. The data path starts at 8 bits; framing and command
semantics should allow a wider physical transport later.

The interface needs defined byte order, packet boundaries, command lengths,
backpressure, reset behavior, and status/error reporting. Provide completion
fences and a presentation command so the CPU can distinguish command acceptance,
render completion, and an actual display swap. Interrupt notification is useful
but its pin allocation is not fixed.

Texture uploads and draws must be ordered so active sampling cannot observe a
partially updated texture. A presentation fence must include pending cache
writeback. With only two color buffers, rendering must wait when a completed back
buffer is queued for presentation and no other buffer is free.

Electrical timing, clock-domain crossing, signal count, GPIO assignment, and
available I/O voltage must be checked against both boards' schematics before
choosing a clock or producing wiring instructions. The computer's pin map is not
present in this repository yet.

## Performance definition

A display pixel, candidate raster position, covered fragment, depth-passing
fragment, and committed color write are different measurements. Report them
separately so empty bounding-box positions or rejected fragments cannot inflate
the advertised fill rate.

For GPU-09, the proposed initiation target applies after pipeline fill to covered
opaque textured fragments with depth testing and writing enabled, a ready color/
depth store, texture cache hits, and no unresolved dependencies. It does not yet
establish sustained one-pixel-per-cycle rendering for arbitrary scenes.

Benchmark large triangles, small triangles, overdraw, texture cache misses, 2D
fills/blits, host uploads, and simultaneous display traffic. Report core and RAM
clocks, shading mode, cache conditions, triangle rate, committed pixels/second,
and stall causes. A sustained-rate requirement needs an agreed scene/workload.

## Decisions still open

- Validate the selected M0 numeric, shading, texture, and 2D profile against M1
  resource/timing measurements; revise it explicitly if needed.
- Choose physical tile banking, resident-tile count, cache sizes, and packed ABI.
- Measure feasible core, SDRAM, and host-bus clocks and choose performance goals
  for representative scenes, including a minimum rendering frame rate.
- Obtain the RP2350-GP-COMPUTER schematic/pin map before assigning the bus.

The [development roadmap](roadmap.md) defines incremental bring-up and validation.
The complete baseline still includes all requested functional features; GPU-09
retains the owner's stated preference status.
