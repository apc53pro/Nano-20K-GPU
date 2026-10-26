# Integrated RTL GPU

The authoritative project guides are now
[GPU_ARCHITECTURE.md](../../GPU_DOCUMENTATION/GPU_ARCHITECTURE.md) and
[GPU_CONTROL.md](../../GPU_DOCUMENTATION/GPU_CONTROL.md). This page provides
additional simulator, regression and qualification details.

The functional GPU core is implemented in SystemVerilog under
[`rtl/gpu`](../../rtl/gpu). Verilator compiles this RTL into
`build/nano-gpu-rtl`, a command-to-pixel simulator with optional SDL2 output.
The existing SRR remains an independent software oracle. Its C++ hardware-block
and timed-system implementations are not linked into the RTL executable.

This is functional RTL simulation and a synthesis entry point for the
**GW2AR-LV18QN88C8/I7**. The production board clock/reset wrapper, HDMI/DVI
serialization integration, pin constraints and complete placed-and-routed
fit/timing qualification remain separate work.

## Device and testbench boundaries

```mermaid
flowchart LR
  HOST["C++: CPU clipping, setup, bins and packet encoding"] --> LINK
  subgraph RTL["Verilator executes SystemVerilog"]
    LINK["Host transport and native asynchronous FIFOs"] --> EXEC["Packet and command execution"]
    EXEC --> TILE["Tile renderer, D24, shading and perspective arithmetic"]
    TILE <--> TEX["Texture cache"]
    EXEC --> MEM["SDRAM service and arbitration"]
    TILE <--> MEM
    TEX <--> MEM
    MEM --> SCAN["Independent video scanout"]
  end
  MEM <-->|"SDRAM commands, address, DQ and DQM"| CHIP["C++: external SDRAM chip model"]
  SCAN --> PIXELS["Captured RGB565 scanout / SDL2 / PPM"]
  ORACLE["C++: independent canonical SRR"] -. "--verify: attachments and displayed pixels" .-> PIXELS
```

The C++ host performs the tasks intended for the RP2350: scene preparation,
triangle setup, ordered binning, resource serialization, bus transfers and
reply handling. GPU storage can change only through RTL memory interfaces.
The external chip model responds to the controller's physical SDRAM commands
and data pins; it does not accept device-side C++ memory requests. Testbench
inspection of memory is used only to check results.

The core implements all eight provisional simulation opcodes:
`GET_CAPS`, `GET_STATUS`, `WRITE_HEAP`, `SUBMIT_PASS`, `COPY_RECT`, `FENCE`,
`PRESENT` and `READ_MEMORY`. The rendering path includes triangles, rectangles,
lines, keyed sprites, the five shading modes, perspective-correct nearest
textures, RGB565 attachments and D24 depth. The command-to-memory path includes
packet buffering, fetched descriptors/records/bin lists, attachment transfers,
copies, SDRAM initialization/refresh/arbitration and independent scanout.
These are implemented paths; scene comparisons do not by themselves constitute
exhaustive coverage of every command, malformed packet or recovery sequence.

Native synchronous RAM wrappers preserve registered read latency and bind to
Gowin `SDPB` primitives in synthesis. The reciprocal arithmetic uses the
existing native multiplier/ROM implementation. The device's native primitive
parameters and memory-port contracts apply in both simulation and synthesis.
See [the module interfaces](../../rtl/gpu/interfaces.md) for ports, handshakes
and the physical SDRAM boundary.

The RTL host transport has a distinct `host_clk` domain and native asynchronous
FIFOs. The present full-scene testbench drives that clock at one quarter of the
core frequency. That test schedule does not characterize all asynchronous
clock relationships, electrical setup/hold margins, or an RP2350 PIO program.
The logical host ports also do not finalize the board's shared-data-bus pad
mapping or pin assignments.

### Current area-reduced implementation

The CPU byte ABI remains `N20K-SYS-0.1`; all command/record fields, capabilities,
numeric behavior and rendering modes are unchanged. Internal storage and
arithmetic have been restructured to reduce mapped LUT use:

- Seven seed/X-step/Y-step plane triplets use 14 native **512×32 SDPB** banks.
  Registered reads anticipate accepted pixel/row steps and hold on stalls.
  Seed capture adds one setup clock per nonempty primitive/tile invocation;
  sustained fragment acceptance and retirement retain initiation interval one.
- Texture tags use one native bank beside the existing texel bank. Forwarding
  the just-refilled tag preserves hit latency and avoids a same-address RAM
  read/write collision. SDRAM write masks use another synchronous bank beside
  the two payload banks.
- Offset/raster/row stepping shares one adder per edge or plane. The executor
  decodes full raw command words through one 32-bit accumulator, retaining no
  56-byte payload-prefix copy. COPY refetches 14 parameter words from its owned
  packet lease and clips X then Y through one signed 33-bit interval/datapath.
  That phase uses about 128 setup clocks for a nonempty copy. Validated viewport,
  address and transfer values use bounded retained widths; full raw-field
  validation and overflow checks remain ahead of narrowing.
- The fragment path uses the **exact integer-texel UV pipeline**, with
  **18 enabled-edge latency**, initiation interval one and **19 native 18×18
  multiplier equivalents**. A coarse reciprocal seed plus exact residual
  corrections returns exactly the high integer bits consumed after canonical
  signed-Q16 rounding, including its overflow boundaries. Eight native delay
  RAMs replace long-lived payload registers. The generic full-Q16 module
  remains available at 26 edges, but is not the GPU's fragment path.
- Fragment contexts use three native banks: 34-bit address/depth metadata in
  one 512×36 `SDPX9B`, a 24-bit factor in one 512×32 `SDPB`, and a 16-bit result
  bank. The distinct retirement and factor-read schedules keep separate ports.
- Three logical 1,024-byte packet leases occupy two physical 2,048×8 banks.
  Full raw header words are validated before retaining small fields. The
  executor uses the immutable queue head's opcode/sequence/length directly.
  Upload masks occupy words 32..47 of the existing upload bank; synchronous
  mask/data staging streams one u32 every three core clocks.
- Ordered queries serialize immutable capabilities directly. Status replies
  retain changing counters and flags while foreground execution holds other
  fields stable, preserving the same coherent reply without a duplicate
  512-bit state bank. Sideband status reads use the acknowledged, held core
  bank without a duplicate host-domain copy. Reset and ready/rejection bookkeeping use explicit
  per-slot state rather than wide dynamic write networks.

The total is **46 native RAM banks** (44 `SDPB`, two `SDPX9B`) and **25 native
18×18 multiplier equivalents**: 19 UV, two address, three shade and one edge.
The physical primitive modes and native
latencies are modeled in simulation; these reductions do not replace hardware
storage with simulator-only state.

## Build and run

Follow the [development setup](../development.md#integrated-rtl-simulator).
On Windows, use **MSYS2 UCRT64**:

```bash
pacman -Syu
# If a terminal restart is requested, reopen UCRT64 and run pacman -Syu again.
./scripts/setup-rtl-sim.sh
make rtl-gpu-run
```

The setup installs Verilator, Python, native C++ tools and SDL2, then checks
them. Pulling the repository does not install dependencies. `make rtl-gpu`
only compiles the executable; `make rtl-gpu-run` also opens its SDL2 window.
The Windows executable is `build/nano-gpu-rtl.exe`. See
[Windows setup and diagnostics](../development.md#windows-rtl-simulator).
If GCC runtime conflicts persist after a full MSYS2 upgrade, that guide also
covers the explicit `gcc-libs`/`cc-libs` compatibility-package migration.

On Linux x86_64:

```bash
./scripts/setup-rtl-tools.sh
make rtl-gpu
./build/nano-gpu-rtl --headless --scene showcase --verify --stats build/rtl-showcase.json
```

For SDL2 output, install SDL2 before building, then run:

```bash
./scripts/setup-sdl2.sh
make rtl-gpu
./build/nano-gpu-rtl --scene showcase --verify
```

The first build can take several minutes while Verilator and the C++ compiler
run. Build progress and periodic activity messages appear in the terminal,
and the full output is written to `build/rtl-gpu/verilator-build.log`. Use the
builder's `--verbose` option to stream every compiler message. Check tools
without installing or building the GPU with `./scripts/setup-rtl-sim.sh --check`;
that check compiles and runs a small RTL/C++/SDL2 smoke test.

The builder detects SDL2 with pkg-config and falls back to a headless executable
if unavailable. To select that build explicitly, use
`python3 scripts/gpu-rtl.py --headless-only`.

The executable supports:

| Option | Effect |
| --- | --- |
| `--headless` | Run without SDL2; defaults to one frame. |
| `--scene showcase\|perspective\|overdraw\|opaque` | Select the host-generated workload. |
| `--verify` | Compare all attachments and every complete scanout frame after the first PRESENT with the independent SRR. |
| `--self-test` | Run the headless command, copy, attachment LOAD, scanout and recovery checks. |
| `--frames N` | Limit rendered frames; windowed runs otherwise continue. |
| `--time S`, `--step S` | Select deterministic animation time and its increment. |
| `--stats FILE.json` | Save RTL cycle counts, verification state and the displayed color hash. |
| `--output FILE.ppm` | Save the last complete displayed frame. |
| `--trace FILE.vcd` | Trace RTL signals; complete-frame traces can be large. |
| `--scanout-log FILE.csv` | Record every complete scanout frame, including black or mismatched frames, with cycle, phase and color hash. |

For a frozen two-frame differential check:

```bash
./build/nano-gpu-rtl --headless --scene showcase --frames 2 --time 0 --step 0 --verify
```

`make rtl-gpu-test` runs the command self-test followed by all four scenes,
with two verified frames per scene starting at time 1.25 seconds. Its
`build/rtl-gpu-tests/results.json` records the simulator executable's SHA-256,
per-case logs and scene statistics; a failed run removes any previous passing
summary. `make rtl-block-test` runs the separate native RAM/video, asynchronous
host, tile-renderer, physical SDRAM controller and texture-cache unit checks.
`make rtl-gpu-synth` builds the simulator and synthesizes the same functional
RTL top with `synth_gowin -family gw2a`.

[`scripts/gpu-rtl.py`](../../scripts/gpu-rtl.py) accepts `--build-dir PATH`,
`--output PATH`, `--headless-only`, `--require-sdl`, `--check`, `--verbose`,
`--synth`, and `--run` followed by simulator arguments. `--check` validates the
toolchain with a small compiled simulation; `--require-sdl` makes missing SDL2
an error instead of producing a headless-only executable. The default build
directory is `build/rtl-gpu`; its generated files
include `verilator-build.log`, `build-report.json`, and, when requested,
`gpu-synth.json` and `gpu-synthesis.log`. The build report records tool versions,
source SHA-256 fingerprints, whether SDL2 was enabled and synthesis cell counts
when available. The script rejects inputs changed during its build.

## Verification and cycle evidence

With `--verify`, the testbench renders the same prepared pass through the
canonical monolithic SRR. It compares every pixel of **both** RGB565
framebuffers and the complete D24 buffer with external SDRAM contents. After
presentation, it also checks every complete frame captured from the RTL's actual
scanout pixels, including during the next upload and rendering pass. The expected
front changes after the PRESENT reply in vertical blank. Frames are published
at the final active pixel, avoiding the nine-bit Y counter's wrap during blank.
SDL2 and PPM output use those captured pixels, including any
scanout failure, rather than reading a framebuffer directly for display.

The command self-test exercises all eight opcodes, a 512-byte unaligned
readback, masked heap writes, overlapping framebuffer copies, clipped copies
from a pitched non-power-of-two image, and color/depth LOAD. It also cancels
partial and malformed packets and an active pass, checks epoch restart and
attachment validity, and confirms that recovery preserves every front-buffer
pixel. Separate native-block tests cover RAM collisions, independent host
clocks, physical SDRAM timing and runtime reset during all eight WRITE beats.
The command test also checks 44 unaligned/short upload patterns, more than
512 KiB of sustained uploads while a nonblack front remains visible, and abort
after an upload reaches the physical SDRAM WRITE pins.

An earlier scanout bug caused most of a displayed frame to turn black during
the next upload. The scanout client offered refills too late to benefit from
arbitration priority, and uploads reserved the service before fetching their
payload. Queued display refills and a native 64-byte upload staging bank fix
the starvation. The contested scanout regression fails the old RTL and checks
consecutive frames, including simultaneous completion, acceptance and swap.
The integration suite now requires zero scanout underflows and saves per-frame
CSV logs, so a later correct frame cannot hide an earlier blackout.

The current area-reduced RTL passes the command/recovery self-test and two
rendered frames of each scene, starting at time 1.25 seconds with a 1/60-second
step. Every attachment and every completed scanout after the first PRESENT
agrees with the canonical SRR:

| Scene | Second-frame cycles through FENCE | Change from prior baseline | Nominal ms at 100.8 MHz | Verified scanout frames | Underflows |
| --- | ---: | ---: | ---: | ---: | ---: |
| Showcase | 8,375,976 | +23,300 | 83.10 | 7 | 0 |
| Perspective | 7,422,004 | +20,824 | 73.63 | 7 | 0 |
| Overdraw | 9,724,304 | +15,232 | 96.47 | 8 | 0 |
| Opaque | 9,773,996 | +17,900 | 96.96 | 8 | 0 |

The [current area-optimization baseline](../benchmarks/rtl-fit-optimized.json)
records executable/source hashes, all five integration results and comparison
against the prior cycle counts. The self-test also passes 29 additional COPY
oracle cases and 17 descriptor/LOAD fault cases. Across these scenes the
submission-cycle increase ranges from 0.16% to 0.28%.

The historical [continuous-scanout baseline](../benchmarks/rtl-scanout-fixed.json)
retains the reproducible pre-fix blackout failure, tool versions and earlier
hashes/cycles. The older [initial baseline](../benchmarks/rtl-initial.json) checked
selected post-PRESENT frames and did not catch the intermediate blackout.

The current RTL still uploads textures on every frame, serializes tile
transfers, and performs LOAD and COPY as individual
pixel transactions. The older C++ model's resident-texture cache, primitive
prefetch and overlapping tile writeback have not been ported to this RTL.

These counts belong to the current integrated RTL serialized schedule, with
continuous scanout maintained. Dividing the showcase submission count by
an **assumed** 100.8 MHz gives approximately 83.10 ms;
100.8 MHz is not an achieved whole-GPU clock, and this result does not establish
30 FPS. The source-matched route below reaches 48.26 MHz in the core domain,
so these nominal milliseconds are not achieved FPGA frame times. Further RTL
optimization needs its own measured baseline and must
preserve the independent oracle comparisons.

`core_cycles` includes reset/boot and final display observation;
`last_submission_cycles` covers sending the last compiled pass through its
fence response. `wall_seconds_excluding_boot` is PC execution time and also
includes any enabled software oracle work. None of these reports includes
measured RP2350 preparation time or proves board performance. The simulator's
`color_fnv1a64` is a 16-digit hexadecimal hash of the captured RGB565 pixels.

## Physical resource result

The complete functional logical-core serial probe **places and routes legally**
on the Tang Nano 20K's exact FPGA, but **fails its core-clock target**. The
physical implementation targets **GW2AR-LV18QN88C8/I7**, using the
**GW2A-18C** database. It wraps the complete logical GPU in
[`gpu_probe.sv`](../../rtl/gpu_probe.sv), a nine-pin serial probe that keeps
external inputs controllable and output cones observable. It does not replace
any GPU function or supply the production board wrapper.

The source-matched [resource report](rtl-core-pack.json) and
[physical implementation report](rtl-core-pnr.json) distinguish:

| Resource | Used | Available |
| --- | ---: | ---: |
| Routed occupied physical LUT sites | **20,351** | **20,736** |
| DFF resources, including serial probe and RAM-reserved slots | 6,830 | 15,552 |
| ALU functions, already included in packed LUT sites | 5,744 | 15,552 |
| Native 18×18 multipliers | 25 | 48 |
| Native BSRAM blocks | 46 | 46 |
| Packed LUT sites before flip-flop input placement | 17,005 | 20,736 |
| Necessary minimum physical LUT sites | 20,036 | 20,736 |

ALUs and eight `RAM16SDP4` cells are already represented in the packed logic
total. Gowin flip-flops require an input LUT; some can pair with an existing
LUT/ALU. Of 6,782 active flip-flops, at most 3,751 have that pairing, requiring
at least **3,031 additional input LUTs**. The resulting **20,036 physical-site
lower bound** is a necessary capacity check, not legal-placement proof or a
measurement of remaining physical sites. DFF capacity also includes 48
RAM-blocked flip-flop slots. All 46 native RAM banks are allocated. The serial probe adds at most
198 registers plus its small checksum/control logic and excludes the production
board wrapper.

Reproduce synthesis and exact-part packing with:

```bash
make rtl-gpu-pack
make rtl-gpu-pnr
```

[`scripts/rtl-core-pack.py`](../../scripts/rtl-core-pack.py) writes full evidence
and logs under `build/rtl-core-pack` and the compact versioned report at
`docs/hardware/rtl-core-pack.json`. Source hashes bind the result to its RTL,
probe, constraints and build scripts. The helper returns status **2** for an
over-capacity resource result, including a failed flip-flop input LUT lower
bound. The selected flow uses `synth_gowin -nowidelut` to avoid wide-LUT
placement restrictions, with current inputs unchanged during the measured run.

The final mapping's legal route occupies **20,351 physical LUT sites**,
or **98.14%**, leaving 385 sites in the probe. This is the union of 14,575
explicit LUT sites (including 32 RAM-storage sites), 5,744 ALU sites and 32
further RAM-reserved sites. The earlier 19,775-site packed inventory failed
placement because its additional required flip-flop input LUTs pushed physical
demand beyond capacity; packed counts alone were insufficient.

The source-matched route applies independent core/host constraints:

| Domain | Requested target | Reported constraint | Routed maximum | Result |
| --- | ---: | ---: | ---: | --- |
| Core | 100.8 MHz | 100.80645 MHz | **48.26255 MHz** | **FAIL** |
| Host | 25.2 MHz | 25.20034 MHz | 78.88924 MHz | PASS |

Reported constraints reflect 1 ps period precision: 9.920 ns core and
39.682 ns host. The core's **20.720 ns, 21-LUT critical path** passes through
command-controller feedback, SDRAM arbiter ready and accepted/pending-memory
recovery logic into synchronous state control. The next timing work is to
pipeline that feedback while preserving the accepted-transfer/abort race
checks and complete rendering/scanout regressions.

The physical runner
[`scripts/rtl-core-pnr.py`](../../scripts/rtl-core-pnr.py) retains source/artifact
hashes and constraints in [rtl-core-pnr.json](rtl-core-pnr.json).
`make rtl-gpu-pnr` enables `--require-timing` and **returns status 2 for the
current core-clock miss despite successful legal routing**.

Packed totals alone do not establish placement, routing or a whole-GPU clock;
the physical-trial evidence is reported separately. The simulator intentionally reports
`whole_gpu_fit_qualified=false`: a capacity pass for the logical-core probe is
not qualification of the final board. Full-device placement, routing and timing
must include the production wrapper and must stay within the remaining
physical resources while preserving command, attachment and actual-scanout
conformance. A routed serial probe remains narrower evidence than a production
board top, so its report always leaves `whole_gpu_fit_qualified=false`.

The complete board still needs clock/reset generation, SDRAM clock/pad timing,
video encoding/serialization integration, the agreed RP2350 bus pads and final
pin constraints. Complete synthesis, placement, routing and timing must cover
that final top, followed by board validation. The standard video serializer
does not need a new GPU rendering proof, but its wrapper and clocks still need
to be included in physical implementation.

The older [reciprocal/UV qualification](rtl-qualification.md) remains evidence
for its arithmetic probe only. Its routed frequency, LUT/FF count and probe
constraints cannot be attributed to the new full functional core. Likewise,
the C++ [storage map](storage-map.md) is an allocation budget rather than a
measured complete-RTL resource report.
