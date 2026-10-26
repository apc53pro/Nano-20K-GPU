# Nano-20K-GPU

A custom GPU for the RP2350-GP-COMPUTER, built with the Gowin Tang Nano 20K FPGA.

The feature baseline is 640×480 RGB565 video through the HDMI connector,
hardware triangle rasterization, perspective-correct textures, two color
framebuffers, at least 24-bit depth, shading, and 2D drawing. The RP2350 performs
triangle setup initially and communicates over an 8-bit parallel bus. A rendering
rate of one pixel per core cycle is the design target. The modeled warm-cache
textured path now meets that cadence in controlled resident-tile tests;
complete-system throughput and FPGA timing are qualified separately.

Video-only DVI-compatible signaling at approximately 60 Hz is confirmed sufficient.

The first software reference renderer is implemented. It uses CPU triangle
setup, 32×32 tile-local rendering, RGB565 color, D24 depth, perspective-correct
textures, shading, and 2D drawing. SDL2 displays the result; a separate headless
build produces deterministic images and raw color/depth dumps.

![Software reference showcase](docs/images/software-reference.png)

## Build and run

Requires a C++17 GCC/Clang compiler with `__int128`, make, pkg-config, and Python 3
for the tests. See the [setup guide](docs/development.md) for platform details.
On Windows, use **MSYS2 UCRT64**, update it with `pacman -Syu`, and then run the
commands below. The installer provides the native compiler, SDL2, pkg-config,
Python, and make. [Full Windows setup](docs/development.md#windows-with-msys2).
MSYS2 is a separate installation from Git Bash; both can display `UCRT64`, but
Git Bash does not include MSYS2's package manager.

```bash
./scripts/setup-sdl2.sh
make -j4
make test
./build/nano-gpu-ref
```

The Windows executable is `./build/nano-gpu-ref.exe`; keep using the UCRT64 shell
so SDL2 and compiler runtime DLLs are available on PATH.

Without SDL or a desktop:

```bash
make -j4 headless test
./build/nano-gpu-ref-headless --headless --verify --output build/frame.ppm
```

Choose `--scene perspective` or `--scene overdraw` for additional workloads.
The [renderer guide](docs/software-reference.md) describes controls, frame dumps,
verification modes, and the C++ API.

The FPGA block backend renders the same scenes through modeled hardware blocks
using the verified Gowin primitive configurations:

```bash
./build/nano-gpu-ref --hardware-profile
./build/nano-gpu-ref --scene showcase --mode blocks --verify
```

Use the `.exe` executable in MSYS2 or the headless executable with `--headless`
on a server. The backend supports all five shading modes, perspective textures,
D24, 2D drawing, keyed sprites, and ordered copies. The selected pipeline uses 46 signed 18×18 multiplier equivalents and 14 native
BSRAM banks: three tile attachments, one texture cache, six fragment-context
banks, and four reciprocal-seed ROM banks. Perspective reconstruction shares a
reciprocal between U and V and corrects the result to exact signed Q16 rounding.
A bounded 64-context pipeline overlaps fragments; cache hits and RGB modulation
have initiation interval one. The [hardware
profile](docs/hardware/gowin-profile.md) and [block interfaces](docs/hardware/block-interfaces.md)
record the source revisions, selected modes, ports, and simulation limits.

The complete system simulation adds the 8-bit host bus, bounded command FIFOs,
packet decoding, SDRAM initialization/refresh/arbitration, fetched primitive
records, and independent scanout with a bounded display FIFO:

```bash
./build/nano-gpu-ref --scene showcase --mode system --verify
```

All uploads, record fetches, texture misses, and framebuffer transfers in this
mode use the same timed SDRAM controller. SDL displays the pixels actually
consumed by scanout, including underflow fallback. HDMI/TMDS signaling is outside
scope. The [system model guide](docs/hardware/system-model.md) separates verified
device facts from configurable timing assumptions and explains the counters.
The system model includes the pipelined rendering datapath. Sustained cache-hit
throughput and complete-pass throughput are measured separately; SDRAM misses,
record fetching and tile transfers still introduce waits.

The system backend now reuses resident textures, prefetches the next primitive,
and overlaps word-streamed tile writeback with rendering through two existing
physical bank sets. Reproduce cold and steady measurements with:

```bash
make system-benchmark
```

This verifies all four scenes against the canonical renderer and checks the
recorded cycle baseline. At the assumed 100.8 MHz core and 25.2 MHz host clocks,
the frozen showcase's steady upload/render submission takes 32.4 ms, compared
with 82.6 ms before these changes. This excludes RP2350 preparation and
presentation waits; the full FPGA clock and fit remain unqualified. The
[measurement details](docs/hardware/system-model.md) distinguish these costs.
The [further optimization review](docs/hardware/optimization-review.md) records
measured experimental improvements and the hardware and protocol work needed
before adopting them.

Exact complete-GPU resource use and physical fit are **not yet established**:
only the reciprocal/UV arithmetic unit has RTL qualification. Run
`make resource-report` for the native allocation budget, source-matched
arithmetic evidence and missing RTL inventory. `make fpga-fit` currently fails
with an incomplete-proof report; it cannot promote a partial arithmetic result
to a whole-GPU fit claim. The [resource-proof requirements](docs/hardware/resource-proof.md)
define the complete RTL, synthesis, routing and timing work still needed.

The throughput harness tracks fragment acceptance and retirement on each
modeled edge, checks ordering and cancellation, and distinguishes sustained
datapath cadence from complete-pass throughput. It can run without SDL:

```bash
make -j4 throughput
make throughput-test
./build/throughput-bench --workload textured-hit --output build/textured-hit.json
```

System runs report bounded throughput measurements in their JSON. Add
`--trace build/edges.csv` to stream a CSV row for every core edge; the trace is
optional and can be large. The [measurement guide](docs/software-reference.md#fragment-throughput-and-clock-traces)
explains the counters, controlled benchmarks, and trace format. Bulk system
staging now has explicit native memory ports and latency, using **39 of 46 BSRAM
blocks** in the selected map. This allocation remains subject to synthesis and
timing qualification.

Warm textured triangles accept and commit 1024 consecutive fragments with
41-cycle latency; warm sprites do the same with 15-cycle latency. The
[measurement guide](docs/hardware/throughput.md) separates those local results
from memory misses and complete-pass costs. The
[RTL qualification report](docs/hardware/rtl-qualification.md) records the
shared-reciprocal prototype separately from the complete GPU.

## Project documentation

- [Requirements](docs/requirements.md): the requested features, confirmed decisions,
  proposed details, and acceptance criteria.
- [Architecture notes](docs/architecture-notes.md): hardware constraints, memory and
  bandwidth budgets, and logical pipeline.
- [Development roadmap](docs/roadmap.md): architecture decisions, feature stages,
  dependencies, and milestone exit criteria.
- [M0 specification](docs/spec/README.md): selected tiled architecture, numeric
  rules, command model, memory layout, and conformance vectors.
- [Software reference renderer](docs/software-reference.md): implementation,
  demos, tests, and output formats.
- [Gowin hardware profile](docs/hardware/gowin-profile.md): exact device resources
  and the modeled multiplier/BSRAM configurations.
- [SRR block interfaces](docs/hardware/block-interfaces.md): clocked rendering
  blocks, arithmetic, texture caching, copies, stalls, and resets.
- [GPU system model](docs/hardware/system-model.md): timed host commands,
  shared SDRAM, scanout, completion, recovery, and provisional simulation ABI.
- [Current GPU pipeline diagrams](docs/hardware/pipeline.md): detailed system,
  rendering, and tile/memory views, with scalable figures and a three-page PDF.
- [Throughput validation](docs/hardware/throughput.md): controlled workloads,
  per-cycle fragment tracking, cadence checks, and JSON/CSV reports.
- [Reciprocal arithmetic](docs/hardware/reciprocal-pipeline.md): exact shared-reciprocal
  reconstruction, native partial products, and error bounds.
- [RTL qualification](docs/hardware/rtl-qualification.md): reproducible simulation,
  synthesis, and routing of the arithmetic prototype.
- [Selected storage map](docs/hardware/storage-map.md): native BSRAM allocation,
  synchronous ports, register fields, and resource limits.
- [SDL2 development setup](docs/development.md): reusable installer and headless
  RGB565 dependency check.

Current status: the M0 specification, initial canonical renderer, CPU setup
library, conformance tests, demo scenes, and a rendering block backend with
matching color/depth output, plus a timed GPU system simulation, are available. Independent geometric
error analysis and command-trace capture remain M0 work. Complete GPU RTL, a
stable packed bus ABI, and measured board performance remain future milestones. Software frame
times do not establish FPGA throughput or fit.
