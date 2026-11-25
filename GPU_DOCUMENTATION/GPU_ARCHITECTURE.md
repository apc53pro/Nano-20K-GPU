# GPU architecture

This is the architecture guide for the **current SystemVerilog GPU** in this
repository. Read [GPU_CONTROL.md](GPU_CONTROL.md) for the CPU-facing bus,
registers, binary commands, data structures and generic host-code examples.

The current RTL uses **`N20K-SYS-0.3`**, a provisional wire ABI. The earlier
published programming image uses version 0.2. See the control guide for
packet and record versions and the required host migration.

**Current status:** rendering and CPU control execute functional RTL under
Verilator, with an independent software reference renderer (SRR) checking the
results. The board target now includes clocks/reset, bidirectional host pads,
registered SDRAM pads, video serialization and exact Tang Nano 20K constraints.
The complete board implementation report, rather than the earlier logical-core
probe, is the authority for board resource and timing results. Hardware
qualification remains incomplete. The MPC-2350 firmware drives the GPU with
one PIO state machine and shares its GPU client between the terminal and the
focused NARM application. Its rendering regression uses the software oracle.
The board image runs a **50.4 MHz GPU core/SDRAM clock** with **25.2 MHz
pixels** and 126 MHz DDR serialization. The GPU's rendered output drives HDMI;
the user reported the terminal running on an earlier image. The previous
image added the SKIP load flag to the pipelined SDRAM controller, word-wide
display FIFO and tile clearing during stores; the user reported
`demo_3d_engine6` running on it with the RP2350 at a stable 60 fps. The
image after it added **GPU triangle setup** for kind-4 vertex triangles
(section 6). The image after that added **early SDRAM command release** and
overlapped tile stores (section 9); the user measured it with
`demo_3d_engine9` (section 13). The published image adds **row exit**
(section 6), **overlapped PRESENT** (section 10) and five timing fixes
(section 12). It is built with Gowin EDA V1.9.8.11, which reached a faster
clock than the signed V1.9.11.03 on these sources; `AGENTS.md` records that
choice and the gate this flow cannot meet. Its build verifies every package
pin and meets every required clock, with the core at 60.36 MHz Fmax. It uses
**19,841/20,736 logic (Gowin count, 96%)**, **7,496 registers**, **46/46
BSRAM**, 63% DSP and one PLL. Little logic is left for new features.
Section 12 notes one uncommitted build-script input. The current image was
written to the board on 2026-11-13; it has not yet been tested with the
RP2350.

The current RTL transfers two RGB565 pixels per accepted SDRAM word and
clears two tile pixels per core clock (section 5). Aligned rectangle
samples also use those paired attachment ports during rasterization
(section 6). Texture refills write two RGB565 texels per accepted SDRAM
word (section 8). Prepared-triangle offsets advance by up to four pixels
per core clock while preserving exact plane values (section 6). Triangle
edge initialization takes nine clocks instead of 24, and kind-4 depth setup
uses one exact radix-four divider for both gradients (section 6). The
published programming image and its reports describe the earlier RTL.
Sections 12 and 13 give the current build and performance evidence at
the 50.4 MHz operating clock.

Version 0.2 replaces raw heap uploads with one lossless encoded upload engine.
It shares the existing packet-word register, counters, 64-byte staging RAM and
SDRAM-write backend; its only additional decoder storage is an eight-bit group
descriptor. All resource types use this engine, with literal words inside the
same encoding. There is no legacy 0.1 packet path or uncompressed fallback.
Version 0.3 keeps the same upload encoding. Decoded rectangle and sprite
records occupy 80 bytes and line records occupy 88 bytes. Prepared triangles
use 320 bytes and vertex triangles use 64 bytes.
The [earlier standalone decoder experiment](../docs/hardware/upload-codec.md)
was a different probe with additional measurement registers; its failed
co-residency result is historical, not an integrated fit measurement. The
historical unified-engine logical-core probe routed with **159 physical LUT
sites remaining**. That count excluded the production board wrapper and must
not be treated as board headroom. Both historical probe reports below retain
their original source hashes; they do not qualify the changed board RTL.

Whenever the architecture or CPU control changes, update **both files in
`GPU_DOCUMENTATION/`** in the same change and push them with the implementation.
Describe implemented behavior as implemented, and label proposals as planned.

**Hardware compatibility policy:** backward compatibility must never consume
additional hardware resources. Retain no legacy decoder, compatibility mode
or duplicate datapath solely to support an older interface at a resource cost.
Break the ABI and update host software, tests and both guides together when
necessary. Compatibility with zero additional hardware cost is allowed. This
policy does not remove required current functionality or input validation;
the currently implemented profile remains unchanged.

## 1. Target hardware and selected operating profile

The target board is the **Sipeed Tang Nano 20K**, fitted with the Gowin
**`GW2AR-LV18QN88C8/I7`**. The open-source implementation tools select the
**`GW2A-18C`** device database and that exact package/part. On the received
board, the open-flow GPU image's host address latch failed while the same RTL
built by the vendor Gowin EDA worked (section 14). The published image
uses Gowin EDA V1.9.8.11 Education under the exception in `AGENTS.md`.
Its 2026-11-13 addendum selects the installed version with the fastest
qualifying route. The current RTL selects V1.9.8.11 (section 12).

| Physical resource | Device limit |
| --- | ---: |
| LUT4 resources | 20,736 |
| Flip-flops | 15,552 |
| Native BSRAM blocks | 46, each 18,432 physical bits |
| Native 18×18 multiplier equivalents | 48 |
| PLLs available in QN88 | 2 |
| In-package SDR SDRAM | 8 MiB, 32-bit data interface |

The default logical-core simulation profile assumes a **100.8 MHz core/SDRAM clock** and
**25.2 MHz host and pixel clocks**. These are nominal design assumptions, not
achieved whole-GPU FPGA frequencies. The host link has its own clock domain;
the RP2350 supplies `host_clk`. The integrated demo drives it at one quarter of
the core clock, and separate link tests exercise other clock ratios/phases.

The **fixed `N20K-PIN-0.1` wiring reservation** connects a Waveshare Core2350B
(16 MiB flash, 8 MiB PSRAM) to Tang Nano 20K hardware 3921 using **16 signal
GPIOs plus common ground**. It reuses VGA GP16/GP17 for HCLK/WR_N and VGA
GP18–GP25 for the bidirectional data byte. The user's 2026-11-06 RP-side wiring
revision replaces unavailable GP10–GP15 with **GP35–GP40 on P4**: GP35 ABORT,
GP36 GPU_RESET_N, GP37 ALE_N, GP38 RD_N, GP39 WREADY and GP40 RVALID.
The `N20K-PIN-0.1` Tang package-pin allocation is unchanged; no RTL or FPGA
constraint change is needed for this host-side remap. GP39 shares the Core's
active-high red LED and must be an RP input, with the LED load driven by Tang
WREADY rather than an independent RP LED output. All selected FPGA banks run
at 3.3 V.
The [control guide's exact wiring table](GPU_CONTROL.md#fixed-wiring-layout-core2350b-to-tang-nano-20k)
and [printable board layouts](../docs/images/core2350b-tang-nano20k-wiring.pdf)
identify manufacturer header contacts and package pins.

[`tang20k_top.sv`](../rtl/board/tang20k_top.sv) implements that wiring:
seven-bit address latching on ALE_N writes, active-low read/write controls,
shared-data output enable, WREADY/RVALID gating and GPU_RESET_N logic reset.
For a packet byte, WREADY combines a registered address-is-zero flag with the
host link's register-only readiness: FIFO space, no sticky host error
(registered separately from the error code) and no flush. This gives the same
value as the core's `host_wr_ready` while RD_N is high, but keeps the address
decode off the half-cycle WREADY pad path (2026-11-13).
[`tang20k.cst`](../rtl/board/tang20k.cst) fixes the external package pins.
[`tang20k-sip.json`](../rtl/board/tang20k-sip.json) records the expected fixed
SDRAM terminals. The current Gowin build's encrypted netlist prevents that
connection check. Font upload, copies and scanout exercise the SDRAM on the
board.

The MPC-2350 bus driver claims one **PIO2** state machine with GPIO base
**16** to reach both control groups and the contiguous data pins. PIO drives
HCLK, write/address/read strobes and data direction, and samples WREADY and
RVALID. The CPU owns packet construction, FIFO deadlines and GPU reset.
Each retry takes **12 PIO cycles per HCLK period**. The firmware derives
the PIO divider from the system clock and one requested HCLK burst rate,
`kHostClockHz`, which defaults to **250 kHz** in the published firmware; the
user's local build uses 18.75 MHz (2026-11-10). Local MPC-2350 firmware
changes (2026-11-10, run on the board) send writes as bursts. WR_N stays LOW,
and the FPGA takes one byte per HCLK rising edge (12 PIO cycles, previously
about 27). PIO samples WREADY before every edge. A byte-wide DMA channel feeds
each burst from an SRAM copy of the packet while the CPU prepares the next
one. Address selects are one-byte bursts; reads remain one transaction at a
time. The earlier
GPIO driver first ran on the board at 50 µs phases. The user reported that
25 MHz failed on their wired setup, and that 21 MHz and 18.75 MHz HCLK work.

The board image uses a **50.4 MHz core/SDRAM clock**, **25.2 MHz pixel clock**
and **126 MHz DDR serializer clock**. One rPLL derives 252 MHz from 27 MHz
(divide 3, multiply 28), with a divided 126 MHz output. Two CLKDIV5 blocks
produce the core and pixel clocks, giving exactly two core clocks per pixel. A
pixel-domain toggle crosses through two core-domain synchronizer stages and an
event detector, and that event advances core scanout. Pixel payloads
therefore change after a physical pixel edge and settle before a later
encoder capture, independent of GPU-reset release phase. The board-wrapper
regression checks capture spacing and exactly one pixel per physical pixel
clock across eight reset-release offsets. The core's rendered pixels drive the
TMDS encoders and HDMI.

From 2026-11-06 to 2026-11-07, a 6.3 MHz bus-test image with a constant-black
raster replaced this profile during bus diagnosis. It is historical and is no
longer built. Clock profiles are compile-time constants: the image contains one
configuration, with no runtime mode or duplicate compatibility hardware. The
host supplies its independent clock, with a design ceiling of 25.2 MHz pending
electrical validation. Board capabilities report **50,400,000 core Hz**,
host-divider metadata 0 (independent clock) and pixel divider 2.

Configuration initializes an eight-bit power-on delay. The board reset button
(pin 88, active HIGH) restarts PLL initialization. PLL lock and GPU_RESET_N
control asynchronous reset assertion and synchronized release separately in
core, host and pixel domains; the native link/FIFOs also synchronize reset
release in their receiving domains. The GPU_RESET_N input has an FPGA pad
pull-down in the package constraints,
so an unconnected host holds GPU rendering in reset. There is no command
player or built-in demo scene.

SDRAM commands, address, masks, DQ output and output enable are registered on
the core rising edge. An ODDR forwards an inverted core clock, giving output
signals approximately half a core period before the chip sampling edge.
The input DQ is sampled at the next core rising edge. Output registers are
not cleared by GPU logic reset, allowing already-issued write tails to drain.
The board waits **10,080 core cycles (200 us)** before SDRAM initialization
and uses refresh interval **787 cycles (about 15.6 us)** with the controller's
early-service guard. CAS3 and BL8 use the controller's default bank timings
(tRP/tRCD 3, tRFC 9, tRAS 6, tRC 9 cycles). These are implemented logical
timings; pad/interconnect margins still require physical-board measurement.

The layout leaves the Tang LCD FPC disconnected and avoids HDMI, flash, SDRAM,
SD, audio and BL616 signal conflicts. WR_N/RD_N/ALE_N use three existing LED
pins as FPGA inputs, with their LED loads included in the wiring/current
instructions. Core GP39 adds its red LED and 470 Ω path to ground to WREADY;
that load remains part of the pending external electrical validation.
The VGA DAC must be disconnected before its GPIOs become a
bidirectional GPU bus. Power, pulls, boot isolation and driver bring-up
limits are specified in the control guide.

The current user-reported assembly uses **A2 RP2350 silicon**, one **100 Ω
series resistor per signal wire** (16 total), and separate **4.7 kΩ pull-downs
from RP GPIO39/WREADY and GPIO40/RVALID to common GND**. Resistor-end placement
is not known. The added pull-downs did not change the all-zero capability
readback; other external bias fitment is unconfirmed. A2's E9 input leakage can
affect weak/high-impedance lines; a valid low-impedance FPGA-driven read should
overcome it. E9 alone has not been established as the cause. The control guide separates
this observed assembly from recommended bias. Raspberry Pi reports E9 fixed
in A4; the `RP2350B` package name alone does not identify the stepping.
[Official A4 announcement](https://www.raspberrypi.com/news/rp2350-a4-rp2354-and-a-new-hacking-challenge/)

## 2. CPU and GPU responsibilities

| RP2350 / CPU side | FPGA / GPU side |
| --- | --- |
| Transform objects and vertices; supply lighting as constant or vertex RGB | Evaluate prepared edge and attribute equations |
| Clip and project geometry; snap coordinates and normalize winding | Test triangle coverage at pixel centers |
| Prepare fixed-point edge/depth/color/UV equations | Perform perspective-correct texture coordinate reconstruction |
| Allocate textures, records and bin lists in GPU memory | Sample the texture cache and fetch misses from SDRAM |
| Bin primitives into ordered 32×32 tile lists | Apply shading, color key, scissor and depth operations |
| Serialize uploads and commands; handle replies/recovery | Load/clear, render and store tile attachments |
| Choose the writable physical buffer and request presentation | Swap at vertical blank and continuously scan out the front buffer |

Vertex transformation, clipping, projection and lighting are **CPU work in
this version**. For example, Gouraud mode interpolates RGB values supplied by
the CPU; the FPGA does not transform normals or calculate a lighting model.
Triangle setup is CPU work for kinds 0..3. For a flat **kind-4 vertex
triangle** the GPU computes it instead: winding, back-face culling, edges,
bounds, depth plane and per-tile rejection (section 6). This record is 64 bytes
instead of 320, and it needs no exact CPU tile test.
The MC10 Minecraft port's textured world triangles deliberately use a binary32
setup instead of the exact rational equations. Its frames are therefore not
bit-identical to the reference oracle. The
[control guide](GPU_CONTROL.md#preparing-triangle-records) states its
measured error bounds.

The C++ host/setup implementation is a reference implementation for preparing
valid commands. The RP2350 PIO driver and **40×30 monochrome terminal** are
implemented in the [MPC-2350 firmware](https://github.com/apc53pro/MPC-2350/tree/6b4ebda7e0bffb24ea81aead5970b014f4146e2c).
That terminal uploads a
64 KiB RGB565 bitmap-font texture to heap `0x258000` once per hard-reset session.
It builds the first frame from 16×16 glyph copies. It tracks which characters
each of the two colour buffers shows. For a changed frame, it picks the
cheapest way to bring the back buffer up to date. Either it draws only the
glyphs that differ from what that buffer already holds, or it first copies the
front buffer with one `COPY_RECT`, shifted up by whole text rows for a scroll,
and then draws the rest. It then fences, presents and waits for the
vertical-blank reply. The terminal uploads a 6×9 RGB565 mouse arrow at
`0x268000` and copies each opaque row span with `COPY_RECT`. The glyph cache
restores cells under an old arrow, including an arrow moved by a shifted front
copy. A stationary terminal sends no packets. Font decoding uses a 512-byte
CPU row, and the GPU holds both pixel framebuffers.

The terminal and the focused NARM application share one GPU session, including
command ordering, sequence numbers and front/back ownership. Native programs
with the GPU capability can upload assets to `0x280000`–`0x7fffff`, submit
passes, copy rectangles, fence, present and read back memory through the
native GPU interface. NARMCC exposes named built-ins for these operations,
record encoding, and link recovery. The firmware serializes records and the
application prepares geometry and bins, manages resource lifetimes, and
draws its pointer. MMBasic stages the next frame's draw records in CPU memory
while the current pass renders. It uses blocking PRESENT for the swap, which
waits for vertical blank. The draw SDK's pipelined frame loop instead sends
each PRESENT with `NARM_GPU_PRESENT_BEGIN`, then the next frame's uploads and
pass, and reads the PRESENT reply later (section 10; control guide). Its host draw SDK tracks tiles that may differ
between the two color buffers and synchronizes only those tiles for
incremental updates. The editor paints each background once, then uses
`COPY_RECT` to place RGB565 source strips from a VRAM cache. The strips are
keyed by the visible glyph bits, colors and scale and can be copied to a
different row without uploading them again.
[MMBasic presentation and editor implementation](https://github.com/apc53pro/MPC-2350/commit/92b9dc7f904df436c18d5b2f3f4bcb3ccccef985)

On return to terminal
focus the terminal drains any pending presentation and rebuilds its cached
view of the buffers. See the [native interface in the control guide](GPU_CONTROL.md#narm-gpu-interface).

The firmware-client host test checks displayed pixels through packet readback
using the clocked software oracle. The board results below cover the earlier
GPIO terminal. The new PIO transport and NARM graphics require an on-board test.

The RP driver keeps **GPU_RESET_N HIGH after a normal host/session failure**, with
the data bus released and read/write/address strobes inactive. The earlier
LOW-on-error path also held scanout and the TMDS serializers in reset, masking
the original bus fault as missing video. Explicit initialization/recovery
still pulses reset LOW and waits **1000 ms with data released before any host
clock or transfer**, allowing time for FPGA flash configuration. A 2026-11-06
report shows that at cold power-up the FPGA had not finished configuring
within this allowance: the first startup read all `FF` with idle WREADY/RVALID
HIGH, while startup after an RP-only reset reached the configured image. The
firmware therefore treats the allowance as a minimum. Until the immutable
capability prefix answers, it re-pulses reset and retries every 100 ms, for up
to **10 s** from the start of initialization. The prefix stands in for a
configuration-DONE signal, which is not read. On 2026-11-08 the user reported
that the first boot after a full power cycle now connects. If the Tang starts after that deadline,
explicitly retry once it has configured. The offline `GPU_LATCHTEST`
diagnostic also pulses reset, without the configuration wait. USB diagnostics retain the startup fault snapshot and
show reset/handshake GPIO levels. Host mocks cover failure handling.

**Board result, 2026-11-07:** the Gowin-built 50.4 MHz image runs the
terminal on HDMI with the GPU's rendered output. Cold starts initially read
all `FF` before the FPGA finished configuring. Later all-zero capability
reads on the configured image were isolated to the open-source FPGA flow's
implementation of the address latch, as recorded below and in the control guide.
The client validates the fixed capability prefix before polling ready status and
offers an offline immutable-capability probe (also automatic after failed
startup) plus RP address-pad echo to help
diagnose the bus. This limited observation is not board timing qualification.
Printing all sixteen probe bytes means the RP software sampled WREADY and
RVALID HIGH during the transfers; their LOW values after the bus is released
are normal idle observations, not evidence that they stayed LOW throughout.
The offline-only `GPU_SWAPTEST`, also run after a completed sixteen-byte
capability probe, refreshes status through register `0x01`, then reads requested
HEX addresses `02`, `21`, `23` and prints these bytes with the raw flags. A
capability-transfer timeout skips the automatic test; the manual command
remains available. With fresh-reset epoch 1 and the always-zero fault-code upper
bytes, expected HEX output is `1 0 0`; a pure D5/D6 interchange in both address
and return data instead gives `1 2 40`. That pattern would strongly support the
interchange under these assumptions, without uniquely diagnosing it; `1 0 0`
does not establish that every line is correct. The output also gives, for
registers `01 02 21 23`, the HCLK rising edges until RVALID was sampled HIGH and
RVALID before the first edge. A genuine handshake gives `edges=4 1 1 1;
pre=0 0 0 0`. Register `0x01` always requests a status snapshot, whose
acknowledgement crosses the core clock domain. An Icarus simulation of
`nano_host_link` shows it answering after the fourth edge. Register-`01`
`edges=1` means that RVALID did not come from this handshake, or that the FPGA
latched another address. The 2026-11-06 report, from firmware without edge
counting, was sixteen zero capability bytes and swap-test `0 0 0`, flags `0`,
with no timeouts. That matches neither pattern. Register `02` uses neither D5
nor D6, and every read returned zero, including nonzero capability bytes. A
D5/D6 interchange alone therefore cannot explain it. A later report, with the
FPGA configured, gave `edges=1 1 1 1; pre=0 0 0 0`. During a confirmed held
read of `0x41`, the Tang measured RVALID pin 74 at 3.3 V, WREADY pin 73 at 0 V,
and all eight data pins at 0 V. The GPU therefore answered and drove `0x00`
instead of `0x02`. This indicates, without isolating the cause, that the FPGA
latched a different capability-range address from the one sent. Address
acceptance cannot reveal this, because address-phase WREADY is `!gpu_d[7]`. The
offline `GPU_LATCHTEST` reset-and-read sequence and the edge-free `GPU_DRIVE`
address-phase hold test that path without any FPGA change. On 2026-11-07 the
open-flow image gave `GPU_LATCHTEST` `0 0 0 0` (first read 4 edges, status
path), while `GPU_DRIVE 55`/`2A` delivered every bit correctly to its Tang pin.
A gate-level simulation of the synthesized netlist passed, and a pin-identical
bus-echo image built by the same open flow worked on the board. The unchanged
RTL built by Gowin EDA reached `READY` with correct latch, capability and status
reads. The address-latch fault is therefore specific to the open-flow
implementation of this design, after synthesis, and is not yet isolated to one
stage. The control guide's address-path isolation table lists the evidence.
Startup can accept register 64's expected zero before
failing register 65, which should return `0x02`. D5 remains GP23 / Core P3.9 to
Tang J5.5 / pin 42, and D6 remains GP24 / Core P3.12 to Tang J5.6 / pin 41;
Arduino data indexing remains `GPIO18 + bit_index`. RP pad echoes do not verify
those endpoints. This host-only diagnostic changes no FPGA circuit or image,
submits no packet, refuses while online or a read is held, and releases the bus
when complete or on timeout. All zeros or timeouts leave the following voltage
test relevant.
An offline `GPU_HOLD` diagnostic first samples idle RVALID with HCLK LOW and
HIGH (both should be LOW while RD_N is HIGH), then selects immutable register
`0x41` (expected `0x02`) and holds HCLK HIGH, RD_N LOW and RP data pins as inputs for stable
voltage measurements. It remains held even for incorrect data or a read-valid
timeout; an address-write timeout releases the bus. `GPU_STATUS` samples the
held data without clock edges, `GPU_PROBE`, `GPU_SWAPTEST` and `GPU_LATCHTEST` refuse while held, and
`GPU_RELEASE` ends the hold. `GPU_RESET` clears it and performs normal hard
initialization. This adds no FPGA circuit or packet retry and leaves the
client offline. The exact measurement points are in the control guide.
If that prefix check fails first, the printed `GPU BOOT` snapshot has not been
read and remains zero-initialized; it does not establish that the GPU has no fault.
The initial transport now uses Arduino `digitalRead()`, `digitalWrite()` and
`pinMode()` for all bus data and control, with Arduino timing calls. The host
mock checks all 256 write/read byte patterns and direction changes, including
Arduino-Pico's output-latch clearing during `pinMode(OUTPUT)`. The read strobe
stays active and HCLK stays HIGH throughout the eight data-pin samples. This
API simplification has not yet been verified on the physical bus.

The current desktop setup code uses `__int128`; it is not a
drop-in library for the 32-bit M33. A firmware port must implement equivalent
bounded arithmetic and preserve the numeric contract. The generic C99 examples
in the control guide explain command encoding, not a completed Pico SDK driver.

## 3. Complete data path

```mermaid
flowchart LR
    CPU["CPU: transform, clip, setup, ordered tile bins"] --> HOST["CPU: heap allocation, upload encoding and packets"]
    HOST --> PADS["Physical 8-bit data bus + controls / address latch"]
    PADS --> LINK["8-bit host link / asynchronous native FIFOs"]
    LINK --> PARSER["Packet decoder / bounded command queue"]
    PARSER --> EXEC["Command executor / shared upload decoding"]
    EXEC --> UP["64-byte upload staging"]
    UP --> MEM["Shared SDRAM service"]
    EXEC --> FETCH["Fetch pass, bins, references and primitive records"]
    FETCH <--> MEM
    FETCH --> TILE["Tile clear/load and rasterizer"]
    TILE <--> LOCAL["Native color + D24 tile RAM"]
    TILE --> FRAG["Depth, perspective UV, shading and ordered commit"]
    FRAG <--> TEX["Texture cache"]
    TEX <--> MEM
    FRAG --> LOCAL
    LOCAL --> STORE["Tile stores"]
    STORE --> MEM
    EXEC --> COPY["Ordered color-copy engine"]
    COPY <--> MEM
    MEM --> FIFO["Two-bank native display FIFO"]
    FIFO --> SCAN["Independent 640×480 scanout"]
    MEM <--> SDPADS["Registered SDRAM output pads / inverted forwarded clock"]
    SDPADS <--> SDRAM["8 MiB ×32 SDR SDRAM"]
    PIXEL["Pixel-domain toggle / synchronized event"] --> SCAN
    SCAN --> VIDEO["Core RGB565 + sync / board TMDS serializers / HDMI"]
```

All GPU data enters through the host interface and reaches external memory
through the SDRAM controller. There is no device-side C++ shortcut for fetching
textures, records or attachment pixels in the Verilator simulator.

The host retains three logical 1,024-byte packet leases in two physical
2,048×8 native RAMs, alongside the separate RX/TX FIFO banks. Two leases may
be published and one staged. Header validation streams complete raw fields
through one 32-bit word before retaining their bounded metadata. The executor
uses the queue head's immutable opcode, sequence and length until retirement,
rather than duplicating those fields or the payload prefix in registers.

The CPU controls an **ordered command processor**, not a bank of persistent
scene-object registers. A primitive record contains the state for one draw.
Updating a sprite record does not automatically move pixels already drawn into
a framebuffer.

## 4. Memory layout

Addresses are little-endian **byte offsets in GPU SDRAM**, not RP2350 pointers.
The fixed 8 MiB address space is:

| Region | Start | End, exclusive | Bytes | Row pitch |
| --- | ---: | ---: | ---: | ---: |
| Physical color buffer 0 | `0x000000` | `0x096000` | 614,400 | 1,280 |
| Physical color buffer 1 | `0x096000` | `0x12C000` | 614,400 | 1,280 |
| Shared depth buffer | `0x12C000` | `0x258000` | 1,228,800 | 2,560 |
| CPU-managed resource heap | `0x258000` | `0x800000` | 5,931,008 | Resource-specific |

Color is RGB565: red bits 15:11, green 10:5 and blue 4:0. A color pixel is at
`color_base + 1280*y + 2*x`. Depth has 24 meaningful bits in a 32-bit storage
word at `0x12C000 + 2560*y + 4*x`; the upper eight bits are zero and do not
implement stencil.

The heap holds textures, primitive records, ordered reference lists, the tile
table and pass descriptors. The CPU owns allocation, non-overlap and lifetime.
The GPU validates supported address ranges, alignment and extents; it does not
maintain a general-purpose allocator or allocation registry. Resources used by
an executing pass must remain immutable until that work completes.

Keep reusable texture, font, sprite and image data resident in the SDRAM heap.
The host tracks these addresses across frame boundaries and allocates
transient geometry and bins separately. Per-pass sampler tag invalidation
does not remove resident SDRAM data. The RTL demo uses the host pass
compiler's persistent texture allocations and reports how many frames upload
texture bytes. The verified two-frame showcase uploads its textures only
for the first frame.

Only ordered heap uploads may write resource memory. Textures cannot sample
the framebuffer/depth regions, and there is no render-target feedback sampling
in the current profile. Diagnostic readback is a command, not a CPU memory map
or the video scanout path.

## 5. Tiling, records and pass execution

The display contains **20×15 tiles**, each **32×32 pixels**, for 300 tiles.

```text
tile_id     = 20*(y/32) + x/32
local_index = 32*(y%32) + x%32
```

The CPU builds a 300-entry tile table. Each entry points to an ordered list of
primitive-record addresses; each primitive occurrence appears in every tile
that can contain its coverage. Conservative bounding-box binning is the
current CPU implementation. Rasterization applies the exact coverage/scissor
test within those bins.

A pass identifies physical color target 0 or 1, independent color/depth
`LOAD` or `CLEAR` operations, a depth `STORE` or `DISCARD` operation, clear
values and the tile table. Those attachment operations apply to the **entire
pass**, including tiles whose lists are empty. `DISCARD` makes depth pass-local
scratch: it is cleared or loaded into the tile as requested and tested
normally, but never written back. The external depth attachment keeps its old
words and becomes invalid, so a later depth `LOAD` faults until a stored
depth clear. Passes that never test depth (2D work) should clear and discard
it; then no depth traffic reaches SDRAM. A pass with `SKIP` leaves every tile
without references untouched in SDRAM, even when it clears; it requires a
valid target color, like a color `LOAD`. The host lists only the tiles that
change, so a frame that redraws a small object costs only the tiles it covers
now or covered before in that buffer.

For each tile the current executor fetches the tile entry, then:

- **Tile without draws.** With `SKIP`, nothing is read or written. Otherwise,
  loaded attachments are already correct in SDRAM, so nothing is read or
  written for them. A cleared color and a cleared depth that is stored use
  constant row writes without touching tile RAM. Each color row is 16 words
  and each D24 row is 32 words. A pass that loads color and discards or loads
  depth skips the tile entirely, so an update costs only the tiles it draws in.
- **Tile with draws.**
  1. Loads each tile row in one 16-word color request and one 32-word D24
     request for the attachments using LOAD. A color word fills both pixels on
     the clock that accepts the SDRAM response. Cleared attachments are
     written two pixels at a time (512 clocks) for the pass's
     first tile with draws; see step 4.
  2. Reads the tile's ordered references in requests of up to 16 consecutive
     words. Lists of one or two references use one word per request through
     the same sequencer. Each burst's first reference goes directly to the
     executor. The shared native COPY row RAM holds the remaining references
     for sequential consumption. Each active pointer is validated before
     fetching its record's first 16-word burst. A kind-4 vertex triangle is
     set up for this tile from those words (section 6).
     The setup can reject a tile without starting the renderer. A prepared
     triangle fetches four more full bursts. Rectangles and sprites follow
     with four words and lines with six. The registered kind selects the
     heap bound and remaining burst length before issuing the transfer.
     A 2D primitive uses its constant depth and skips plane setup. Retained
     triangle coefficients and edges are irrelevant to a following 2D draw.
  3. Runs the primitive in the tile and drains its fragment work before
     changing draw state.
  4. Streams each tile row back to SDRAM in one 16-word color request and,
     unless depth is discarded, one 32-word D24 request. Each accepted clock
     supplies one RGB565 pair or one depth word.
     The tile RAM holds its output while the SDRAM payload slot is not ready.
     The next clock can clear the previous read address while fetching a
     different one. Color clears in pairs. Depth clears during its own store,
     or two pixels at a time with color when depth is discarded. Later tiles
     of the pass use these prepared clear values.

The GPU has **one resident tile attachment set** of 1,024 RGB565 and 1,024
D24 pixels in three native 512×32 RAMs. The color bank stores consecutive
pixels in the low and high halfwords. A fragment writes its halfword
through the native byte enables. A paired rectangle writes either or both
halfwords according to the two depth results. Each depth bank holds whole
D24 pixels of one parity. A single-pixel read registers the requested parity
to select the depth value, and an aligned rectangle pair can read both banks
for two independent comparisons. Both banks can write the same clear or
rectangle depth value in parallel. Full framebuffers remain in SDRAM.

List order defines visible overlap/depth behavior. Different primitives in one
pass may carry different supported states; changing state does not require a
separate pass. Geometry, interpolation and coverage must not depend on tile
boundaries or stalls.

## 6. Rasterization, interpolation and perspective

Screen X increases right and Y increases down. Triangle vertices are snapped
to Q4 subpixel positions; samples lie at pixel centers. The CPU normalizes
submitted triangles to positive area. A top-left edge rule gives shared-edge
ownership without cracks or double coverage.

The CPU supplies signed 64-bit `{seed, step_x, step_y}` planes. The renderer
initializes them at each tile's clipped sample start and advances them through
clocked additions. They retain the primitive's original anchor; there is no
independent rounding of a new plane for every tile. For a prepared triangle,
the offset walk advances first in Y, then in X. If every enabled slope on
an axis fits signed 33 bits, it takes four steps per clock while at least
four remain. The remainder and axes with wider slopes use single steps.
The coefficient loader checks the high word to select the batch size.
Shifting an eligible slope left by two fits signed 35 bits, so only the
low 35 adder input bits select between the ordinary and shifted slope.
All updates use the existing signed 64-bit accumulator and overflow
check. Starting from an in-range value, equal signed steps are monotone.
An in-range exact endpoint proves the skipped intermediate additions
also fit. A failure on one axis stops setup before rasterization. The
controller prefetches the next coefficient at each axis boundary.

Triangle edges are evaluated at the start by multiplication. A triangle
with no enabled plane (flat color, no depth or texture) skips the offset
walk. Kind-4 setup supplies an already offset seed. Rectangles, sprites
and lines use constant depth and skip the walk.

The rectangle walker issues two samples on one clock when its current X
is even and the next pixel is inside the same clipped row. Odd initial
pixels and single-pixel tails use one issue. An aligned pair shares the
constant RGB565 and D24 values. With depth testing enabled, it reads both
parity banks at the same local word address and compares each stored D24
value independently. An accepted lane writes only its enabled attachments.
The existing pending sample stage retires both lanes before the next
primitive. Clip boundaries, row transitions, reset and fault suppression
follow the same rules for single samples and pairs. This uses the existing
three attachment RAMs.

Edge initialization uses one signed 18-by-18 multiplier with its existing
three registered stages. It accepts the six `A*X` and `B*Y` products for
the three edges on consecutive clocks, while the earlier results drain.
The controller combines each pair with that edge's `C`, using the existing
signed 38-bit sum and signed 32-bit overflow rejection before rasterization.
This takes nine clocks rather than the previous six serial four-clock
operations. It changes neither coverage nor record validation, and adds no
multiplier or RAM. The same path handles prepared triangles and the prepared
words generated from kind-4 records. The separate kind-4 setup unit computes
depth gradients before emitting those words.

| Quantity | Stored plane scale |
| --- | --- |
| Depth in D24 code units | 16 fractional bits |
| Gouraud R, G, B channel codes | 16 fractional bits |
| Normalized reciprocal W, `q` | 37 fractional bits |
| Perspective attributes `U=u*q`, `V=v*q` | 27 fractional bits |

Each plane's seed, X step and Y step are stored in a pair of native
**512×32 SDPB banks**, for **14 coefficient banks**. Header and edge fields
remain directly retained registers. Each pair uses addresses 0/1/2 for
seed/X-step/Y-step and supplies one 64-bit coefficient per plane per clock.
Registered coefficient reads anticipate
the next accepted raster step, including row changes and backpressure, rather
than inserting a read bubble between pixels. The seed capture adds one setup
clock for a nonempty primitive/tile invocation; it does not change the sustained
fragment cadence.

Draw setup is split across retained viewport, scissor and tile intersections,
followed by counter/source preparation. Reusing the interval registers adds
three setup clocks per valid invocation compared with the area baseline,
including valid empty windows. Structural and full-width sprite-span checks
complete before texture configuration or attachment access. A structural fault
can appear two setup edges later; completion and fault polling remain the CPU
contract. The registered empty-window flag still permits a present texture
descriptor to be configured and validated even when there are no pixels.

One shared signed 64-bit adder per plane handles offset, horizontal and row
steps. Each edge uses a shared signed 28-bit adder for its upper bits.
Pixel steps add multiples of 16 to the signed 32-bit edge value, so the low
four bits carry through unchanged. The overflow checks use those same
sums. Quantization still implements exact round-to-nearest, ties-to-even
and saturation while avoiding arithmetic on bits that cannot affect the
bounded result. Full-width command and address
validation precedes narrower retained positions and transfer counters; storage
reduction does not authorize truncating invalid inputs into valid ones.

Validated non-line tile spans contain 1–32 candidates per axis. Five-bit
remaining-column/row counters retain the number after the current candidate,
in `0..31`, and supply row-end and penultimate-sample lookahead directly.
They advance only with the rasterizer and hold on stalls; the line walker
retains its separate Bresenham behavior. Full raw bounds validation and
viewport intersection still precede these bounded counters.

**Row exit (2026-11-12).** An edge whose X step is zero or negative never
improves to the right. Once the current sample fails such an edge, every
later sample of the row fails it too. When at least two candidates remain
after the current sample, the renderer can finish the row early. The exit
clock prefetches the Y coefficients. The `ROW_SKIP` clock takes the
ordinary row step from the saved row start. The earlier row-exit benchmark
removed about half of the samples walked outside triangles (section 13).
The exiting sample still takes a speculative X step so the plane and edge
register enables can stay independent of coverage. The row step replaces
those values, and the discarded X sum's overflow flag is ignored.

**Uncovered-sample stepping.** A triangle can advance over five uncovered
samples in two clocks when there are at least five candidates after the
current one in the same clipped row. The first clock takes its ordinary X
step. The second, `RASTER_SKIP4`, takes four X steps together, leaving the
following candidate for the ordinary coverage test. At the fifth sample
one rising edge must still fail and every non-rising edge must pass.
Monotonicity proves the batch is entirely uncovered and preserves every
existing row-exit decision. The renderer enters the batch only with its
pending pixel and textured fragments drained and with every enabled X
slope in signed 33-bit range. Prepared triangles and triangles from GPU
vertex setup use the same rule. The batch reuses the offset adder's
shifted plane operands and prefetches the Y coefficient if it lands on
the final candidate of a row. Scalar steps, paired rectangle steps,
uncovered batches and row exits share the same coordinate and attachment
advance.

Existing edge setup also captures each X threshold `-A`. At four pixels
ahead the edge equation is `E + 64*A`. Comparing the signed quotient
`floor(E/64)` with the threshold identifies the sign. At equality the
remaining six bits and the top-left tie flag decide coverage exactly.
The batch updates the full edge and plane values. It issues no fragment,
depth read or attachment write for the five uncovered samples.

Each plane and edge adder has a sticky overflow flag. The renderer checks
only consumed additions. An exact endpoint within range proves that
every intermediate same-axis addition fits, even when an uncovered
sample still requires those additions. A row exit retains its original
discarded X step behavior. The public fault asserts on the edge that
detects overflow, lowers busy and suppresses later attachment writes and
fragment acceptance. A batched offset can fault before rasterization.
An uncovered batch can fault after earlier fragments have retired and
before any subsequent sample is accepted. The controller enters its
failed state on the following edge.

The current setup domain limits a triangle's W ratio to 64 and vertex UVs to
`[-1024,1024]` texels. Out-of-domain geometry needs CPU refinement or rejection.
Range checks precede narrowing; overflow is not permitted to wrap silently.

### GPU triangle setup (kind 4)

[`nano_triangle_setup.sv`](../rtl/gpu/nano_triangle_setup.sv) sets up flat
vertex triangles. It captures the record's first 16 words as they stream from
SDRAM: three Q4 vertices, three D24×256 depths, the drawing bounds, flags and
color. For each tile reference it then writes the renderer's ordinary kind-0
command words, so the tile renderer is unchanged:

1. **Window.** The record's bounds are intersected with the tile. An empty
   window ends the reference. The bounds act as a scissor; the CPU normally
   supplies the pixel-center triangle bounds it already computed for binning,
   so the GPU needs no min/max tree.
2. **Orientation.** Four products evaluate edge 0 at vertex 2, which is the
   signed area. Zero area, or a negative area with the record's cull bit, ends
   the reference. Otherwise a negative area negates every edge. That is the
   edge set of the triangle with vertices 1 and 2 exchanged, with inclusive
   bits derived from the negated signs, so no vertex is moved.
3. **Edges and tile test.** Per edge, two products give `C = -(A*Xe + B*Ye)`,
   written straight to the renderer. Two more evaluate the edge at the window
   corner where it is largest. If any is negative, no pixel center can be
   covered and the reference ends. Six more products (`A*1`, `B*1`) write the
   edges' A and B words.
4. **Depth numerators.** Since `A0+A1+A2 = 0`, `Nx = Z0*A1 + Z1*A2 + Z2*A0` and
   `Ny = Z0*B1 + Z1*B2 + Z2*B0` need only three products each and no depth
   subtractors. The divider takes `|Nx|`; `|Ny|` waits in a register.
5. **Gradients.** One radix-four restoring divider runs for each axis in
   turn. Each two-bit step brings down two dividend bits, then subtracts
   the greatest of `0`, `|area|`, `2*|area|` and `3*|area|` that fits.
   The full path checks saturation, emits 21 pairs and one final rounding
   bit, then rounds and clamps the result. When the high 26 bits of the
   numerator magnitude are strictly below `|area|`, the first eight
   quotient bits are zero. The divider starts from the exact remainder and
   unused dividend bits after that prefix, bypassing four two-bit steps
   and the initial check. Equality uses the full path. Each result is
   `|N|*4096/|area|`, rounded half away from zero and saturated at `2^42-1`.
   Its sign is that of `N*area`. Between the two divisions, the X gradient
   words and the seed's X term are formed. Meanwhile the header, window,
   scissor and inclusive words stream to the idle renderer. The bounds equal
   the window, so the renderer's offset walk is zero.
6. **Seed.** The Y term, rounding constant and `Z0*256` complete the depth at
   the window corner, `Z0*256 + floor((SX*Gx + SY*Gy + 8)/16)`. The seed and
   Y gradient then fill their words.

One signed 44×16 multiplier (two-clock latency), one 64-bit accumulator and
one divider compute every product, sum, magnitude, rounding and sign.
Constants and stored values reach the accumulator as products (`q*1`,
`G*±1`, `Z0*256`, `1*8`). The multiplier's first operand is therefore one of
only four sources (coordinate, depth, quotient or 1), and its second operand
is an edge coefficient, a sample offset or a constant. Each issued step
carries its accumulator operation down the multiplier pipeline, and a
follow-up action reads the settled result. A depth-free triangle skips steps
4–6. Vertex records replace four of the five record bursts and the offset
walk. Vertex positions use 14/13-bit fields, so X up to 16383 and
Y up to 8191 form a right/bottom guard band; every edge and plane value stays
in range. An earlier draft with two parallel dividers and a wide operand
multiplexer needed 98% of the device's logic and could not be routed; the
board build is the authority for this one's fit. [`vertex_triangle_setup()`](../reference/src/setup.cpp)
is the arithmetic contract. A pixel's depth is the same whichever tile
computes it, because neighboring seeds differ by whole-pixel steps. The C++
decoder expands a kind-4 record into the ordinary triangle the oracle renders.
Malformed fields fault the pass with code 6.

For textured triangles the FPGA reconstructs `u=U/q` and `v=V/q`. Depth stays
screen-affine and is not divided by q. The selected shared-reciprocal pipeline
uses native multiplier/ROM resources. The fragment path selects the
**19-enabled-edge integer-texel pipeline**, with initiation interval one.
It returns exactly `floor(RNE(U/q in signed Q16)/65536)` and likewise for V,
including the same invalid-q and signed-Q16 overflow boundaries. It uses a
coarse reciprocal seed followed by exact residual corrections; it omits the
Newton step because it computes the consumed texel coordinates directly.
It does not approximate texture addresses or relax the input domain.

Positive partial-product limbs use native unsigned 18-bit mode; residual
products use mixed unsigned/signed mode. The private texture-coordinate path
uses **19 native 18×18 multiplier equivalents** and eight native RAM delay
banks. Their circular addresses advance with the complete pipeline and hold
on backpressure, preserving latency and initiation interval without retaining
all delayed payloads as flip-flops. The two 43×35 U/V reciprocal products are
**retimed** (`nano_uv_mul` `RETIME=1`). Their six DSP blocks take operands
without input registers, because both operands are already advance-enabled
register outputs. The shifted partial-product sum is registered instead. The
latency stays three enabled edges, and the wide adder tree no longer shares a
clock period with `integer_estimate` window selection, which closes 50.4 MHz
in the board build. The generic full signed-Q16 reconstruction
module remains available with **26 enabled-edge latency**, but is not the
fragment path instantiated by the complete GPU.

Normalization selects and retains its mantissa and rounding predicate before
applying rounding and exponent carry correction on the following edge. This
adds one enabled edge to the private path while preserving initiation interval
one. The input attribute guard also prepares `q + 32` with its input token;
the next stage compares against `(q + 32) << 5`, exactly the previous
`q*32 + 1024` limit, without a limit-adder carry before that comparison.

The final texel rounder registers the q-dependent thresholds one stage before
the result: `epsilon = q >> 17` uses 21 bits and `q - epsilon` uses 38 bits.
The two signed result candidates are formed in parallel, with
bounded range predicates preserving the asymmetric signed endpoints
`-32768` and `32767`. The final remainder comparison selects a candidate;
it does not feed a serial increment/negate/range-comparison carry chain.
The final-round retiming preserves exact results, overflow flags, the 19-edge latency
and initiation interval one.

Depth/color conversion uses round-to-nearest, ties-to-even, then clamps to the
supported code range. The complete equations, clipping rules, numeric bounds
and texture-boundary convention are in the supporting
[rasterization contract](../docs/spec/rasterization.md). They remain part of
the input contract; this overview does not relax them.

## 7. Supported draw states and restrictions

| Primitive | Shading | Color key | Depth and scissor |
| --- | --- | --- | --- |
| Triangle | Flat, Gouraud, texture replacement, texture×flat RGB, texture×Gouraud RGB | Textured modes | Supported |
| Rectangle/pixel | Flat RGB | Unsupported | Constant D24 and scissor supported |
| Line | Flat RGB | Unsupported | Constant D24 and scissor supported |
| Sprite | Raw 1:1 texture replacement | Optional | Constant D24 and scissor supported |
| Color copy | Raw RGB565 copy | Unsupported | Leaves depth unchanged; copy clipping applies |

For draws, color-write, depth-test and depth-write enables are independent.
Supported comparisons are `LESS`, `LEQUAL` and `ALWAYS`. Disabled depth testing
passes independently of depth-write enable. A depth-only draw can still need
texture sampling when its key determines whether depth may be written.

For a covered fragment, the renderer determines depth, performs the selected
comparison, samples/shades if required, rejects the raw key if enabled, then
commits the enabled color/depth writes. A rejected/keyed fragment changes
neither attachment. Ordered retirement and draining between primitive state
changes preserve draw order and depth visibility.

The current implementation does not provide alpha blending, stencil, MSAA,
bilinear filtering, mipmaps, multitexturing, fog or a programmable shader.
Textures are RGB565 with power-of-two dimensions from 1 through 1024 and
validated pitch/extent. U and V independently select clamp or repeat; sampling
uses nearest texels after perspective reconstruction.

Sprites have no scaling, rotation, flipping, Gouraud modulation or persistent
position register. Moving one means changing its destination, updating bins as
needed and redrawing the affected back buffer. Its texture may remain resident.
A sprite/cursor overlay composed during scanout is a possible future feature,
not an implemented capability.

Scaled, rotated, flipped or tinted images need no sprite hardware. A textured
triangle maps any texel coordinates, so two of them draw such a sprite, with
color key and depth. The MPC-2350 NARM draw SDK does this in
`gpu_draw_sprite_ex()` (2026-11-15; [control guide](GPU_CONTROL.md#narm-gpu-interface)).
The cost is two prepared triangle records instead of one sprite record, with
nearest-texel sampling.
Each tile walks the triangles' bounding boxes, so the corners of a rotated
quad are walked as well. With the board's logic 96% used (section 12),
there is little room for dedicated rotation hardware.

Supported states are composable within those limits; the full cross-product
has not been exhaustively verified. One known validation gap remains: the CPU
encoder rejects keying on an untextured Flat/Gouraud triangle, but a directly
submitted raw RTL record can have that invalid key flag ignored. It is not a
supported draw configuration.

## 8. Texture and fragment pipelines

The texture cache is direct mapped with **64 lines of 16 RGB565 texels**.
One native 512×32 data bank holds eight little-endian texel pairs per
line. The second native bank holds the 12-bit tags. The read edge latches
the texel's parity alongside its response tag, so either halfword can use
the same seven-edge hit pipeline with initiation interval one. Two
registered 18×18 multipliers form pitched texture addresses.

One bounded miss is retained at a time. Each accepted SDRAM response word
writes two texels directly to the data bank on that edge. A full line
uses eight writes. A short final line fills its remaining words with
zero without extending the SDRAM request past the allocation. The line
becomes valid only
after all writes and successful completion of the declared response.
Refill-tag forwarding avoids a same-address RAM collision or replay
bubble for the retained demand. Younger work waits while the shared SDRAM
service refills the line.

Texture configuration has two preparation edges before the final ready/valid
acceptance: allocation/field classification, then the bounded allocation-end
check. The producer holds its descriptor through that handshake. Active
descriptor fields and cache validity change only after a successful final
acceptance; withdrawing an unaccepted configuration keeps the old descriptor
and tags. An explicit flush still invalidates tags. Configuration
preparation leaves hit latency, hit initiation interval and bounded
refill scheduling intact.

The textured fragment path retains up to **64 contexts**, using three native
RAM banks: a 512×36 `SDPX9B` bank holds the 34-bit attachment address/depth
metadata, a 512×32 `SDPB` bank holds the 24-bit RGB factor, and a 1024×16 bank
holds the shaded result. Retirement and factor lookup retain separate native
read schedules. Tags preserve correspondence through
perspective reconstruction, texture fetch and shading. Three native
multipliers calculate RGB modulation. Raw RGB565 key comparison occurs before
modulation and before a depth commit.

Input capacity uses the retained depth stage's downstream space rather than
the current depth comparison. Rejected samples still leave that stage, but
when the downstream path is blocked a replacement can arrive one edge later.
Warm passing and rejecting streams retain initiation interval one. This keeps
the depth-RAM comparison out of the rasterizer's input-ready control path.

The coordinate handoff to the cache is a two-entry elastic buffer: the main
output register and one spill register each retain coordinates, context tag
and RGB factor together. Upstream space depends on the registered spill-valid
bit, breaking the combinational cache-ready path back to the rasterizer's
step enables. A full spill entry can delay upstream restart by one edge after
backpressure clears; buffered requests still leave on consecutive ready
edges. This elastic handoff itself adds no warm output stage and preserves
initiation interval one.

The tile instance retains signed 16-bit texel coordinates within its validated
domain; the generic fragment module defaults to signed 32-bit coordinates.
Prepared command fields and numeric validation retain their existing widths
and rules. The private UV arithmetic has 19 enabled-edge latency.

Controlled warm tests accept and retire 1,024 consecutive fragments at one per
core clock. This establishes the resident-tile pipeline cadence. It does not
make full-frame rendering one clock per displayed pixel: command delivery,
primitive/tile setup, uncovered candidate samples, misses, transfers, overdraw
and scanout contention also consume cycles.

## 9. Shared SDRAM service and upload staging

All accesses share one service with six logical clients:

| Client | Work |
| ---: | --- |
| 0 | Display scanout |
| 1 | Host heap uploads |
| 2 | Descriptors, bin/reference/record fetches and diagnostic readback |
| 3 | Tile attachment clear/load/store traffic |
| 4 | Texture-cache refills |
| 5 | Color copies |

The service maps ordinary 1–16-word transactions and private 32-word D24
tile row transactions onto physical sequential BL8 SDRAM commands. Two native
RAM banks hold its 32-bit payload in **two slots of 32 native word addresses
each**. A third bank holds the four byte-write masks with the same synchronous
read schedule and slot addressing. A D24 row uses the existing addresses
16–31 of its slot. The service still supports two transactions in progress
with the same RAM banks. Masked padding preserves bytes outside a short
write. The controller implements initialization, bank/row timing,
read/write turnaround, refresh and response backpressure. Its selected
physical mode is CAS latency 3 and BL8.
Client 0 has bounded priority; other clients rotate fairly.

A transaction is split into BL8 segments, each one 32-byte-aligned column
block. **Column commands are pipelined**: the next segment's READ or WRITE
issues eight edges after the previous one whenever its row is open, so a
read returns data on consecutive edges and writes stream back to back. Each
aligned 32-word D24 row uses four physical BL8 commands.
The next segment's PRECHARGE or ACTIVE may issue during the current burst,
except that no PRECHARGE truncates an in-flight read. A WRITE segment issues
as soon as its own words are staged and its first word is in the RAM output
register, while the client keeps staging later segments. A write retires with
its last data beat: tWR is enforced per bank before any later PRECHARGE, and
read/write turnaround only separates bursts of opposite direction. Refresh
waits for in-flight bursts, then runs between segments; staging and the read
response stream continue meanwhile.

Read responses stream from the native RAM as soon as captured words are
available, including while the physical BL8 tail or another burst is in flight.
The existing fill counter tracks captured words; a read never consumes an
uncaptured word or reads the location being written on the same edge. Native
RAM output holds its valid word under backpressure. A transaction's
completion (`done`) still waits for client response consumption or, for a
write, its last physical data beat. This reduces scanout refill latency
without another RAM bank or response FIFO.

**Early command release (2026-11-10).** A transaction gives up the command
slot as soon as its **last column command has issued**. Its remaining write
beats, or its read captures and response stream, finish in the background
while the next request is granted, checked, opens its row and stages its
data in the other payload slot. Each slot stays busy until its own
transaction's completion event, and a grant waits for a free slot, so at most
two transactions are in progress. Completion events (`done`, and freeing the
slot) are the final beat of a write's last segment or the final response
handshake of a read; the beat run and the response stream each retain their
own owner and slot. Responses are returned in acceptance order. Direction
interlocks keep the data bus and the payload RAM read port safe:
- a WRITE column command waits until every released read's captures and
  response stream are finished;
- a READ waits for released write beats and for an older read's stream, but
  not for an earlier segment of its own transaction;
- a read capture has priority over write staging on the shared payload RAM
  write port for that edge.

Before this change every transaction retired before the next was granted, so
each one paid its arbitration, check, row activation and staging serially;
early release hides most of that behind the previous burst. It costs about
56 LUT/ALU cells and 26 flip-flops in a Yosys comparison of the service alone.

The payload and mask RAM read address has two sources: the transaction
issuing column commands, and the background beat run. Since 2026-11-13 each
source computes its own read enable and address in parallel, and the column
command decision selects between the two results last. Its row comparison
therefore no longer runs through the beat multiplexers. A simulation-only
check compares every edge against the earlier single expression.

The arbiter computes foreground eligibility from the round-robin cursor in
parallel, choosing the first request at or after the cursor or wrapping to the
first earlier request. Its one-hot grant drives `cmd_ready` directly; binary
owner encoding is used for the selected payload and retained transaction.
The bounded scanout preference and foreground order apply at each grant.
A D24 tile row holds the command slot through its fourth BL8 column command.
The next eligible scanout or foreground request can then be granted when a
payload slot is free. Refresh can still run between the row's physical segments.

An ordinary 1–16-word transaction spans at most 64 bytes. Only the final
64-byte line of SDRAM can cross the 8 MiB limit, so its extent check uses a
six-bit word-offset-plus-count sum there instead of a full byte-address carry
chain. Raw alignment and count checks remain, and an extent ending exactly
at 8 MiB is valid. The private row flag applies only to the granted tile
attachment client, client 3. Its retained request must carry the 16-word
token and a 128-byte-aligned address in the D24 region
`[0x12c000, 0x258000)`. Those aligned region bounds keep the entire row
inside the D24 attachment. Validation converts the token to a 32-word
transaction. Boot initialization and other memory clients use the ordinary
transaction path. The host packet formats and SDRAM layout are unchanged.

The acceptance edge captures owner, address, count and direction. A registered
`CHECK_REQUEST` stage validates those retained fields before collecting write
payload or scheduling SDRAM commands. It adds one startup clock per transaction;
invalid requests report done/error one edge later internally. Ready/valid
acceptance, client ownership and ordered completion semantics are unchanged.

Before requesting a heap-write transaction, the executor collects a whole
bounded **64-byte upload chunk** in a separate native RAM bank. Once accepted,
it streams staged words every **three core clocks**: byte masks occupy otherwise
unused words 32..47 of that same bank and are read before the two data halves.
Slow host byte decoding must
not hold the shared memory owner while the display FIFO drains.

The upload engine decodes a descriptor for up to four u32 words, using zero,
sign-extended 8/16-bit or literal 32-bit values. It reads the owned packet bank
only for encoded bytes, then routes decoded bytes through the existing word
assembly and byte-mask staging. The command accumulator becomes the decoded
word register; readback cursors are reused because upload and readback commands
execute serially. An eight-bit descriptor is the only new retained payload.
Each command expands 1..956 bytes, permitting every input to fit the existing
1024-byte packet limit. Exact encoded consumption and unused descriptor codes
are checked before the final chunk is submitted. Earlier chunks may already
have reached SDRAM when malformed input faults; recovery requires reuploading
the affected resource. No second upload engine or additional BSRAM is present.

Foreground uploads, passes and copies execute in host command order.
Within a pass, a tile's store transfers are offered back to back without waiting for each
completion, and the executor moves on to the next tile once the last one has
been accepted. A tile's LOAD waits until no store is outstanding. At the end
of a pass, a drain state waits for every store to complete before the
attachments are marked valid and the command completes, so FENCE and PRESENT
semantics are unchanged. An outstanding-store counter supplies the
`pending_memory` term used by recovery. The RTL does not implement the C++
system model's primitive prefetch schedule. Color copies work one row at a
time:
1. **Read.** The row's 32-bit source words arrive in SDRAM bursts of up to 16
   words. Each word is realigned on arrival into destination-aligned words in
   one 512×32 native row RAM. With a one-pixel source/destination phase
   difference, a word's low half is paired with the previous word's high
   half, and odd shifted rows add one flush word.
2. **Write.** After the service accepts a burst's final payload word, COPY
   can request the next burst while the earlier write finishes on SDRAM.
   Up to two accepted writes share the service's existing transaction slots.
   Each burst still carries up to 16 words, at one word per accepted core
   edge, with halfword strobes for partial end words.
3. **Drain.** A COPY counter tracks accepted transactions through physical
   completion. Every write of the row must retire before COPY reuses its
   row RAM or reads the next source row. The last row drains before the
   executor marks the target valid or completes the command.

Reading a whole row before writing it preserves same-row overlap. Reverse
row order preserves source pixels during downward overlap. An abort or
fault stops new requests and prevents the interrupted copy from publishing
a valid target or successful completion. Accepted payloads and physical
writes still drain through the existing recovery sequence. Before
2026-11-07, every pixel took a separate one-word read and a separate masked
write, which made a 640×480 copy take well over 0.8 s at 6.3 MHz. A one-cycle
setup state also registers the copy direction and starting destination row
before the address arithmetic, to meet 50.4 MHz timing. The executor retains
no 56-byte command-prefix copy:
it validates complete raw words through one 32-bit accumulator and reuses the
owned native packet lease until retirement. COPY refetches 14 parameter words,
clips X then Y through one signed 33-bit interval/subtract/compare datapath,
and retains only bounded nonempty origins/counts. Since 2026-11-13, each of
the eight interval steps holds its 33-bit candidate in a register, and the
compare and bound update take a second clock, which removes the long
candidate → compare → bound-enable path. That refetch/clipping phase uses
about **136 core setup clocks** before address walking and pixel traffic,
while preserving signed-coordinate clipping, including `INT32_MIN`.

Reference reads stop at the validated end of each tile list. The executor
drains each accepted response before also using the command-fetch port for
the referenced primitive record. Buffered pointers are validated only as
their positions reach execution. For example, a malformed later pointer
faults after its earlier references even if all arrived in a single burst.
COPY and pass execution use the shared row RAM in separate command lifetimes.
A fault or abort drains the accepted burst and discards staged references
before the next pass. The tile table and lists keep their 0.3 layout, limits
and lifetime rules.

Primitive-record fetches have a one-word pipeline between the SDRAM read port
and tile command-word decoding: 32 data bits, a seven-bit word index and valid.
It accepts one word per clock within a burst. The existing record-completion
state drains the final word before draw launch, so no extra fetch state is
needed. Abort, fault, quiescence or renderer reset suppress/cancel a pending
word rather than allowing it into a later draw or epoch.

## 10. Double buffering and display

Successful initialization makes physical color 0 the front and color 1 the
writable back. Both are black, shared depth is `0xFFFFFF`, and the device then
reports ready. Startup produces black at valid logical video timing until
memory/scanout can run.

The CPU renders only to the writable non-front buffer. `PRESENT` executes after
earlier work, seals the requested target and swaps at the next eligible vertical
blank. The old front becomes writable. If a new frame is late, the display
continues reading the existing front instead of exposing unfinished rendering.

**Overlapped PRESENT (2026-11-12).** The executor accepts a valid `PRESENT` at
once: it seals the target, retires the command and records its sequence and
token. The swap still happens at the next vertical-blank pulse, whatever the
executor is doing. Until then, only `WRITE_HEAP` commands execute. Every
other command waits in the queue, including the next pass, which needs the
buffer the swap frees. After the swap, the executor's next idle clock sends
the PRESENT reply. Replies therefore stay in command order, and nothing but
uploads runs between a PRESENT and its swap. The status flags keep busy and
sealed set until the swap. This lets a host send the next frame's uploads
right behind the PRESENT, so they use the vertical-blank wait instead of
following the swap. It costs a 32-bit sequence register and three flags;
the reply engine, token register and swap logic are shared with the
earlier blocking implementation, whose wait state is removed.

Depth is **one shared attachment** and is not swapped. The newly writable color
buffer contains an older frame, while shared depth describes the most recent
pass. A normal new 3D frame clears depth; `LOAD` alone does not make an old color
buffer and shared depth a matched pair. A depth-`DISCARD` pass leaves shared
depth invalid.

Logical core scanout runs independently at 640×480 active pixels in an 800×525
timing frame, with active-low sync. On the board, each synchronized 25.2 MHz
physical pixel event advances scanout by one pixel, giving 60 Hz frames.
Video-only DVI-compatible signaling through the HDMI connector is sufficient;
audio and HDMI data packets are not required.

Two native RAM banks form the **2,048-pixel display FIFO**, organized as 1,024
words: one bank holds each word's even pixel and the other its odd pixel. A
16-word refill is accepted at one word per edge, so it does not hold the shared
SDRAM service while pixels drain; each FIFO word read supplies two pixels.
(The previous FIFO accepted one pixel per edge, keeping the service owned for
about 32 edges per refill: roughly 40% of SDRAM time in a clear pass.) Scanout queues its
next refill before the current transaction retires. With early command
release, **up to two refills are in flight**: the scanout counts accepted
refills whose last word has not arrived, reserves 16 FIFO words for each of
them and for the queued request, and keeps one discard flag per in-flight
refill. Obsolete responses are discarded across front swaps. This keeps it
eligible for memory arbitration during foreground traffic. Underflow produces
black for the rest of that affected frame and increments a diagnostic counter;
it is an error condition, not normal presentation behavior.

The functional core outputs RGB565, sync, display-enable and pixel coordinates.
On the board, its RGB565, sync and display-enable feed the standard TMDS encoder
and four native OSER10 serializers, driving TLVDS_OBUF pads. Lanes 0/1/2 carry blue/green/red at package
pairs 35/36, 37/38 and 39/40; lane 3 is the clock pair 33/34. The CST selects
LVDS25, matching Sipeed's hardware-3921 HDMI example. This is video-only output;
there are no HDMI audio or auxiliary packets.

## 11. Completion, ownership and recovery

Command acceptance means queued; it does not mean the operation is complete.
A fence after rendering completes only after prior execution and stores.
Presentation completes at the swap. Refer to the control guide for the exact
reply identity, sequence and epoch rules.

The area-reduced query serializer retains changing counters and flags at
query execution, then serializes the other foreground fields while ordered
execution holds them stable. Immutable capabilities are serialized directly.
The diagnostic core cycle counter and its reply snapshot are physically
48 bits. The unchanged u64 wire slot zero-extends the upper 16 bits and wraps
modulo 2^48 (about 64.6 days at 50.4 MHz); hard reset restarts it. This storage
reduction does not affect rendering arithmetic or command sequence/epoch widths.
The wire result remains a coherent snapshot. Sideband reads use the
acknowledged core snapshot bank held unchanged until the next request; refresh
blocks reads until the cross-domain acknowledgement. Removing duplicate
512-bit copies does not change status offsets, packet lengths or the command ABI.

An execution fault can leave partial back-buffer or shared-depth contents. The
GPU marks affected attachments invalid, suppresses later success and drains
issued memory operations. A valid front keeps displaying. A full clear pass can
reinitialize invalid attachments; `LOAD` cannot read them as valid data.

Recovery first registers foreground quiescence. An abort waits for the renderer
to become idle before stopping new foreground memory commands, including
texture refills; a fault also establishes this quiescent state. Already
accepted write payloads, read responses and completions continue to drain,
and display client 0 remains active. The controller checks the pending-memory
register on a later edge, so a transaction accepted on the edge that establishes
quiescence is included. This removes same-cycle SDRAM arbitration from the
recovery-state decision. Quiescence stays asserted through the renderer reset
and host flush acknowledgement, and is released only when the recovery state
returns to idle. These additional recovery control edges add no clocks to
ordinary rendering.

Soft abort is out of band. The CPU must stop old transfers, keep the required
clocks running through recovery, discard stale completion assumptions and wait
for the fresh stream/epoch before sending new work. An interrupted heap write
leaves its affected range unspecified; reupload it before reuse. Hard reset
restarts initialization and invalidates the driver's resource/session
assumptions; it does not promise heap preservation.

## 12. Native resource accounting and current fit

Native RAM and DSP operations use the same selected digital interfaces in
simulation and synthesis. In synthesis, the wrappers bind actual Gowin `SDPB`, `SDPX9B`,
`MULT18X18` and related primitives. RAM reads have registered latency; reset
does not clear stored RAM words; forbidden same-address read/write collisions
are checked in simulation.

The logical core RTL declares **46 native RAM banks** as follows. The board
image uses all 46 of the device's banks, because the scanout FIFO is in use.

| Allocation | BSRAM banks |
| --- | ---: |
| Resident tile color/depth | 3 |
| Seven plane seed/X-step/Y-step coefficient pairs, 512×32 mode | 14 |
| Fragment metadata/factors/results | 3 |
| Reciprocal seed ROMs | 4 |
| Texture-cache data and line tags | 2 |
| Host RX/TX and two physical packet banks for three logical leases | 4 |
| Shared SDRAM transaction data and byte-write masks | 3 |
| Display FIFO | 2 |
| Diagnostic reply staging | 1 |
| COPY row and tile-reference staging | 1 |
| Host upload chunk staging | 1 |
| UV payload/context delay rings | 8 |
| **Total** | **46** |

The selected logical modes are **44 `SDPB` and two `SDPX9B`** banks. Additional
logical buffering must reuse or reduce those allocations. The arithmetic
selection uses **25 native 18×18 multiplier
equivalents**: 19 for UV reconstruction, two for texture addresses, three for
RGB modulation and one for triangle edge setup. RAM and DSP subtotals alone do
not establish fit; LUT/register mapping must be measured for the complete core.

### Historical logical-core probe (before board integration)

The following counts and frequencies apply only to the earlier source hashes
in the linked reports. They exclude board PLLs, video/pad logic and SDRAM output
registers; they are not the current board fit. Exact-part synthesis, packing
and routing of that logical core inside its serial qualification wrapper reported:

| Resource | Use or necessary bound | Device capacity |
| --- | ---: | ---: |
| Routed occupied physical LUT sites | **20,577** | **20,736** |
| Packed LUT sites before flip-flop input placement | **17,277** | **20,736** |
| Necessary minimum physical LUT sites | **20,237** | **20,736** |
| DFF resources, including probe and RAM-reserved slots | 7,011 | 15,552 |
| Native 18×18 multipliers | 25 | 48 |
| Native BSRAM | 46 | 46 |

The packed 17,277 sites include 5,558 ALU functions and the sites occupied by
eight `RAM16SDP4` cells. Gowin flip-flops also require an input LUT, which can
sometimes share an already-counted LUT/ALU site. Of 6,963 active flip-flops,
at most 4,003 can pair with existing sites, requiring **at least 2,960 further
LUT sites**. Thus 20,237 is a necessary lower bound, not a legal-placement or
routing result. The DFF inventory also reserves 48 RAM-blocked flip-flop slots.

That historical `-nowidelut` implementation's legal route occupies **20,577 physical LUT
sites (99.23%)**, including FF feed-throughs, ALU and RAM reservations. That
union leaves **159 sites** in this serial probe. Compared with the previous
0.1 route, the integrated upload change adds 272 physical LUT sites and eight
DFFs; BSRAM and DSP counts are unchanged. Do not subtract the lower bound from device capacity and
present that difference as proven free physical sites.
The earlier 19,775-site packed inventory failed placement because required
flip-flop input LUTs raised its physical demand beyond capacity.

The source-matched route applies independent core and host targets:

These frequencies are **static timing estimates from nextpnr's device delay
model**, not measurements of a physical FPGA's maximum clock. Verilator checks
cycle behavior without establishing propagation delays. After synthesis,
placement and routing, static timing analysis combines modeled cell and route
delays with register timing requirements. Its predicted limit can differ from
silicon behavior. Qualification needs Gowin timing analysis for the final board
top and its constraints, followed by hardware tests; this probe does not prove
that hardware cannot exceed the reported frequency.

| Clock domain | Requested target | Estimated routed Fmax | Result |
| --- | ---: | ---: | --- |
| Core | 100.8 MHz | **70.23 MHz** | **FAIL** |
| Host | 25.2 MHz | 120.37 MHz | PASS |

The timing report represents those targets with 1 ps period precision, giving
100.80645 and 25.20034 MHz respectively. The core's 14.239 ns critical path
runs from SDRAM burst-address state to the memory-service write-mask bank.
The previous 0.1 route estimated 68.88 MHz; the current estimate reflects the
new mapping and routing, not a changed clock plan or a measured board clock.
Warm fragment initiation interval one is preserved. Further clock optimization
remains paused by user choice; board clock/reset integration and measurement
are still required. A legal route establishes
physical resource fit for the serial probe, not operation at 100.8 MHz.

The probe adds up to 198 registers plus small control/checksum logic and does
not include a production board wrapper. Clock/I/O integration and its resource
needs must be qualified for that final top.

The source/artifact hashes and tool output are recorded in
[the resource report](../docs/hardware/rtl-core-pack.json), with physical-trial
evidence in [the probe implementation report](../docs/hardware/rtl-core-pnr.json).
The reports retain source/artifact hashes and validate unchanged inputs.
Production-board qualification must include the clock/reset and video/pad
wrappers, final pin constraints and complete clock/I/O timing closure. Neither
a C++ resource allocation nor an isolated arithmetic probe replaces that work.

### Timing fixes and toolchain comparison, 2026-11-13

Gowin EDA V1.9.11.03 Education is now installed. Its `gw_sh.exe` carries a
valid Authenticode signature from Guangdong Gowin Semiconductor.
`scripts/build-tang20k.py` records the version, the signature and whether
the synthesized netlist is encrypted. That netlist is still encrypted
(`pragma protect`), so the SiP connection check remains impossible with
either version.

On the row-exit and overlapped-PRESENT sources (2026-11-12), none of the 15
V1.9.11.03 placement/routing combinations passed the timing gate. The host
clock reached 23.5–24.3 MHz against 25.2 MHz, failing on the address register →
write acceptance → `gpu_wready` pad path. The core reached at most
50.47 MHz, limited by the COPY clipping bound enables and the SDRAM mask-RAM
address. V1.9.8.11 passed the same sources at 52.04 MHz, limited by the row
exit's edge tests feeding the plane register enables. Five RTL changes
remove those paths without changing any image, reply or cycle count of the
rendering benchmark:
- the WREADY packet-byte term reads a registered address-is-zero flag and a
  register-only host readiness (section 1);
- the host link keeps "no host error" as its own register;
- each COPY clipping step registers its candidate before the compare
  (section 9; about 8 more setup clocks per COPY);
- the SDRAM payload/mask read address is computed separately for column
  commands and the beat run (section 9);
- a row-exit sample still takes its X step (section 6).

Both versions then built the same sources, with the same uncommitted 4 ns
host-pad constraints, for placement 0–2 × routing 0–2. The best passing
build of each:

| Toolchain | Options | Core (50.4) | Host (25.2) | Pixel (25.2) | Logic |
| --- | --- | ---: | ---: | ---: | ---: |
| **V1.9.8.11** (unsigned) | 0/2, 2/2 | **60.36 MHz** | 32.98 MHz | 200.82 MHz | 19,841 (96%) |
| V1.9.11.03 (signed) | 0/2 | 56.45 MHz | 27.60 MHz | 195.85 MHz | 19,158 (93%) |

All nine V1.9.11.03 builds passed (core 50.50–56.45 MHz). Of the
V1.9.8.11 builds, three passed, one failed timing (41.68 MHz core) and five
left nets unrouted, a sign of routing congestion at 96%.
Both are limited by the same SDRAM path, from the segment base through the
bank-row comparison. Per `AGENTS.md` (addendum 2026-11-13), the user keeps
the version whose image reaches the faster clock, so V1.9.8.11 is the
default. These Fmax figures are timing margin: the core still runs at
50.4 MHz.

The logic count rose by about 1,500 from the 18,321 of the earlier V1.9.8.11
build. Nearly all of the rise is ordinary LUTs in the tile renderer, which
grew from 3,992 to 5,331. No memory was remapped. A diagnostic build with
the earlier renderer source and the other four fixes counted the renderer at
5,347, and Yosys `synth_gowin` maps both renderer versions to about the same
size (5,565 and 5,469 LUTs). The rise is therefore Gowin's mapping of the
changed netlist, not hardware added by the fixes. V1.9.11.03 counts
the same sources at 19,158. Either way, little logic is left for new features.

### Retained programming-image implementation (2026-11-13)

The [published-image complete-board report](../docs/hardware/tang20k-board.json) is
matched to the 50.4 MHz GPU-video profile described above, with one
exception: it was built with a working-tree `scripts/build-tang20k.py` that
adds host-pad timing constraints from separate, uncommitted work. The report
records both script hashes and the difference
(`uncommitted_build_inputs`). Those constraints add `set_input_delay` and
`set_output_delay` of 4 ns on the falling host-clock edge for the bus pads.
The RTL and every other input matched the sources at that release; this
report does not qualify the later pipelined edge initialization. It was built
by `make tang20k` with Gowin EDA V1.9.8.11 Education (that release's default),
using placement option 0 and routing option 2 (Makefile
`TANG20K_PLACE`/`TANG20K_ROUTE`). The previous comparison shows the other
options; these are build choices, not hardware modes. The full `tang20k_top` is implemented: PLL, two CLKDIVs,
resets, the physical host wrapper, registered SDRAM boundary pads, the
pixel-event handoff, and the TMDS serializers and pads. All 26 package pins,
including the four derived HDMI N pins, match `tang20k.cst`.

| Physical resource | Used | Capacity |
| --- | ---: | ---: |
| Logic (Gowin LUT/ALU count) | **19,841 (96%)** | 20,736 |
| Registers | 7,496 (48%) | 15,915 |
| Native BSRAM banks | 46 (100%) | 46 |
| DSP | 63% | |
| rPLL / CLKDIV | 1 / 2 | 2 / 8 |

Gowin's logic count is not directly comparable with the nextpnr LUT-site
counts of earlier reports. No BSRAM bank is spare. The previous comparison
explains the rise from the previous image's 18,169. The image before that
(triangle setup, without early release) used 18,058, so early release and
overlapped stores cost about 111. The image before the kind-4 setup unit used
16,481, so the unit and its core integration cost about 1,600.

| Board domain | Constraint | Gowin Fmax (slow corner, 0.95 V, 85 °C) |
| --- | ---: | ---: |
| Core/SDRAM | 50.4 MHz | **60.36 MHz** |
| Host | 25.2 MHz | 32.98 MHz |
| Physical pixel | 25.2 MHz | 200.82 MHz |
| Reference | 27 MHz | 406.29 MHz |

The core closes with about 20% margin in this placement, limited by the
SDRAM segment base → bank-row comparison → column-command path. The previous
image reached 59.29 MHz, and the one before it 54.97 MHz. The host figure now includes the constrained half-cycle
pad paths described above; the previous image, without those constraints,
reported 68.18 MHz for internal host paths only. Two core decisions
read registered flags computed when a reference is fetched (the 320-byte
record range and the last-reference test), so neither comparison feeds the
executor's next-state logic. The image before the setup unit closed at
56.46 MHz with options 2/1. The image before that (pipelined SDRAM
without SKIP) closed at 51.67 MHz with a Gowin count of 18,185 logic, although
a Yosys `synth_gowin` of its sources mapped slightly fewer LUTs than before;
Gowin's count varies with its mapping of each netlist. Earlier placements
ranged from 51.13 to 62.8 MHz. Two earlier RTL timing fixes took the core
from 41.2 MHz to above 50.4 MHz:
- the retimed U/V reciprocal multipliers (section 6);
- registering the copy direction and starting row before the address
  arithmetic (section 9).

Eight reset-release paths have negative slack, and the report lists each one:
power-on into the core and pixel reset synchronizers, −3.25 ns and −1.93 ns,
which is an asynchronous preset into two-flop synchronizers that release
synchronously; and the synchronized pixel reset into the four OSER10
serializer resets, −0.26 ns, as listed for earlier images.

The build fails on any other negative-slack path.

`AGENTS.md` records the exception for this flow and the user's choice of the
faster toolchain. The fixed SiP SDRAM connection cannot be checked from
Gowin's encrypted netlist; on the board, the font upload, copies and scanout
of earlier images show the SDRAM working. The V1.9.8.11 copy is unsigned and
its provenance unverified.

The image is **7,264,137 bytes**, SHA-256
`39fc7dd67bec606d59837a5c23e6240df5203548889db74bc1dd1098ff131730`. On
2026-11-13 it was written to the Tang's SPI flash with `make tang20k-flash`
and the signed standalone Gowin Programmer V1.9.12.04; the FPGA reloaded it
from flash with status `0x6020` and user code `0x04A2`. It has not yet been
tested with the RP2350. The
[programming-file check](../docs/hardware/tang20k-bitstream-check.json)
verifies device ID, declared frame count and frame CRCs; this is structural
file validation. The [matching functional evidence](../docs/hardware/tang20k-functional.json)
covers two layers:
- the board-wrapper testbench (773 clock/pad/bus checks with an isolated core
  model);
- five real-core cases at the 50.4 MHz board profile: the
  command/copy/DISCARD/SKIP/overlapped-PRESENT self-test with 44 copy oracle
  cases, plus four rendering scenes.

The previous image (`ecc7030b…608c`, early release, without row exit and
overlapped PRESENT) was written to the Tang's SPI flash on 2026-11-12 with
`make tang20k-flash` and the signed standalone Gowin Programmer V1.9.12.04
(program and verify succeeded; the FPGA reloaded it with status `0x6020`,
user code `0x16D1`). The 2026-11-12 `demo_3d_engine9` board measurement in
section 13 used it.

The image before that (`93739598…f886`, triangle setup without early release)
was written to the Tang's SPI flash on 2026-11-09 with `make tang20k-flash`
(program and verify succeeded; the FPGA reloaded it with status `0x6020`,
user code `0xB6B9`). The 2026-11-10 `demo_3d_engine9` board measurement in
section 13 used it.

On 2026-11-08 the previous image (`5b6f75aa…08e6`, SKIP without the setup
unit) was written to the Tang's SPI flash with
`make tang20k-flash` (program and verify succeeded; the FPGA reloaded it with
status `0x6020`, user code `0x72CD`). With the RP2350, the user measured
`demo_3d_engine6` (draw SDK with SKIP, blocking present, no frame sleep) at a
stable 60 fps without faults. With the earlier MPC-2350 firmware's asynchronous
presentation (`try_present` and `frame_ready` polling), the same demo lost one
byte at the start of a PRESENT reply. The firmware rejected the header and
took the link offline until the RP2350 was reset. The software model passed
with the earlier read order. On 2026-11-09 the user reported intermittent
crashes even after the host read-order change, and up to 10 fps in the
MMBasic interface. The cause of the asynchronous reply fault remains open.
MMBasic now uses blocking PRESENT.

In a board-clock RTL replay of four MODE 5 editor frames without the mouse
cursor, all corresponding 640×480 RGB565 buffers had matching checksums.
A redraw of 24 distinct source lines took 100 ms before the change, 33.3 ms
with the new path when the strips first uploaded, and 16.7 ms when the
strips were resident. These times include virtual host packets and vertical
blank, and exclude RP2350 execution time and physical bus timing.

Earlier that day the previous image (`3fc16700…06ef`, pipelined SDRAM without SKIP) was written
to the Tang's SPI flash with the signed Gowin Programmer V1.9.12.04
(`make tang20k-flash`: program and verify succeeded; the FPGA reloaded it with
status `0x6020`, user code `0xE7A9`). On it, the user measured
`demo_3d_engine6` at 25 fps; an instrumented build showed 12.6 ms of GPU
rendering, 7.8 ms of CPU and bus work and 12.8 ms waiting for vertical blank
per 33.3 ms frame. On 2026-11-07 the burst-transfer image (`a249064f…9ef9`,
user code `0x3354`) was flashed the same way. The control guide lists the exact build and programming tools. Earlier GPIO firmware mocks
verify the 2 µs phase floor. On 2026-11-07 the user programmed the previous
image (SHA-256 `55ebdfbd…d170`, per-pixel LOAD; Gowin Programmer verified the
flash write) and reported it displaying and refreshing the MPC-2350 terminal
on HDMI. The user later reported filemgr running near 1 FPS on it, and the
PIO bus working at a 21 MHz HCLK on their wiring. A
standalone RP2350 sketch also kept the 2 us bus running a continuously moving
screen without locking up. These are functional observations, not electrical
or timing qualification. A keyboard stall seen during these runs was an RP2350
firmware stack overflow, not a GPU fault; the control guide records it.

### Earlier board implementations (historical)

- **Open-source 50.4 MHz image** (nextpnr, seed 3). Its routed estimates were
  63.14 MHz core and 205.42 MHz pixel. Gowin's own timing analysis of the same
  design gave 41.2 MHz before the two fixes above, so nextpnr's delay model
  overstated the margin.
- **Open-source 6.3 MHz bus-test image** (2026-11-06). Its host address latch
  failed on the board, as recorded in the control guide.

Their reports remain in git history and do not qualify the current sources.

### Pipelined-edge source snapshot before divider change (2026-11-24)

The [V1.9.8.11 experiment](../docs/hardware/tang20k-edge-pipeline-v1908.json)
and [signed V1.9.11.03 experiment](../docs/hardware/tang20k-edge-pipeline-v1911.json)
implement the same full `tang20k_top` sources, package and 0/2 placement/routing
options. Both pass the required 50.4 MHz core, 25.2 MHz host and pixel timing
gates and all 26 package-pin checks. The core estimates are 57.699 and
58.251 MHz respectively. The signed tool was the default for that snapshot.
The operating clock for both builds was 50.4 MHz.

The signed build uses 19,606/20,736 logic and 7,421 registers; the older tool
reports 19,903 logic and 7,495 registers for the same sources. Both use all
46 BSRAM and 63% DSP. These complete-board tool counts are scoped to their
respective local snapshots and must not be conflated with a core-only probe
or an architectural resource saving across different toolchains.

Both reports retain every listed negative-slack reset-release path under
the existing exception: two core-reset, two pixel-reset and four serializer
reset paths. No other listed violation is accepted. The unchanged encrypted
netlists prevent the fixed SiP check; external electrical margins and physical
qualification remain incomplete. V1.9.8.11 provenance remains unverified;
the V1.9.11.03 executable's Authenticode signature was separately checked.

The reports fingerprint the same uncommitted 4 ns falling-HCLK pad constraints
already identified for the retained image. That separately owned builder is
not included in this change, so a clean checkout alone is not the qualified
input set. The Makefile passes the chosen tool explicitly; direct builder
invocations must supply `--gowin` to reproduce these toolchain comparisons.
The matching candidate image/checksum bundles remain local experiments;
the published 2026-11-13 image and its reports are retained unchanged.

### Divider-prefix source snapshot before paired tile transfers (2026-11-24)

The [V1.9.8.11 route](../docs/hardware/tang20k-divider-prefix-v1908.json)
and [signed V1.9.11.03 route](../docs/hardware/tang20k-divider-prefix-v1911.json)
use identical full-board sources and placement/routing options 0/2. Both
pass the required clock gates and verify all 26 package pins. V1.9.8.11
reaches a core Fmax estimate of **63.729 MHz** and host **31.900 MHz**. The
signed build reaches **50.595 MHz** core and **25.812 MHz** host. V1.9.8.11 was
the selected tool for this snapshot. Both builds operate
at 50.4 MHz core, 25.2 MHz pixel and 126 MHz serializer clocks.

The selected route uses **20,077/20,736 logic**, 7,495 registers, all 46
BSRAM banks and 63% DSP. The signed route uses 19,931 logic and 7,417
registers, with the same BSRAM and DSP use. Each report lists the eight
negative-slack reset-release paths permitted by the existing gate. Both
record the exact local host-pad timing script and the hash of the
[functional report](../docs/hardware/tang20k-divider-prefix-functional.json).
Local images are
paired with their SHA-256 checksums and verified device ID, frame count and
frame CRCs. The published image remains the earlier release. The encrypted
netlists prevent a fixed SiP check, V1.9.8.11 provenance is unverified,
and external electrical timing still requires physical measurement.

### Paired tile attachments source snapshot (2026-11-24)

Both [V1.9.8.11](../docs/hardware/tang20k-paired-attachments-v1908.json)
and [signed V1.9.11.03](../docs/hardware/tang20k-paired-attachments-v1911.json)
implement the same complete board sources at placement/routing 0/2. The
reports verify all 26 package pins and the clock gates. The old tool
estimates **57.626 MHz core** and **29.039 MHz host**. The signed tool
estimates 52.215 MHz core and 28.484 MHz host. V1.9.8.11 is selected under
the faster-core rule. The board profile continues to run at 50.4 MHz core,
25.2 MHz pixel and 126 MHz serialization.

The selected route uses **18,687/20,736 logic**, 7,479 registers and all 46
BSRAM banks. This is 1,390 fewer logic units than the preceding V1.9.8.11
divider-prefix route. The signed route uses 20,174 logic and 7,403
registers. Both use 63% of DSP blocks. The difference in mapped logic
varies with the toolchain.

The reports give the exact unchanged local host-pad constraints and link
their [functional evidence](../docs/hardware/tang20k-paired-attachments-functional.json)
by checksum. Each lists eight permitted negative-slack reset-release paths.
The two local Gowin programming files passed device-ID, frame-count and
frame-CRC checks and are stored alongside their checksums. The published
image remains the earlier release. The encrypted netlists prevent the
fixed SiP check. The V1.9.8.11 toolchain provenance is unverified and
external electrical margins still require physical measurement.

### Short-record source snapshot (2026-11-24)

Both [Gowin V1.9.8.11](../docs/hardware/tang20k-short-records-v1908.json)
and [signed V1.9.11.03](../docs/hardware/tang20k-short-records-v1911.json)
routed identical complete-board sources at placement/routing 0/2, verified
all 26 package pins and met the board clock and host capture gates.

| Gowin version | Core Fmax | Host Fmax | Logic | Registers | BSRAM |
| --- | ---: | ---: | ---: | ---: | ---: |
| V1.9.8.11 | 58.664 MHz | 31.444 MHz | 18,441/20,736 | 7,436 | 46/46 |
| V1.9.11.03 | 54.193 MHz | 27.694 MHz | 19,445/20,736 | 7,358 | 46/46 |

The selected V1.9.8.11 route uses 246 fewer logic units than its matched
paired-attachment predecessor and retains the 50.4 MHz core/SDRAM,
25.2 MHz pixel and 126 MHz serialization clocks. Each report lists
the remaining permitted reset-release violations and records the same
uncommitted host-pad timing script and
[functional evidence](../docs/hardware/tang20k-short-records-functional.json).
The two local image checksums are paired with their source, device, frame
count and frame CRC evidence. The earlier 0.2 programming file remains
the published image. Encrypted netlists prevent a fixed SiP check, the
V1.9.8.11 installer provenance remains unverified, and external electrical
margins still require measurement.

### Paired-rectangle source snapshot (2026-11-25)

Both [Gowin V1.9.8.11](../docs/hardware/tang20k-paired-rectangles-v1908.json)
and [signed V1.9.11.03](../docs/hardware/tang20k-paired-rectangles-v1911.json)
routed the same complete board sources at placement/routing 0/2, verified
all 26 package pins, and passed the required board and host-capture timing
gates.

| Gowin version | Core Fmax | Host Fmax | Logic | Registers | BSRAM |
| --- | ---: | ---: | ---: | ---: | ---: |
| V1.9.8.11 | 60.095 MHz | 30.224 MHz | 20,167/20,736 | 7,437 | 46/46 |
| V1.9.11.03 | 50.494 MHz | 25.818 MHz | 20,093/20,736 | 7,359 | 46/46 |

V1.9.8.11 remains selected and leaves 569 of 20,736 logic units in this
route. It uses 1,726 more mapped logic units than the preceding version's
same-tool route. The selected profile stays at 50.4 MHz core/SDRAM,
25.2 MHz pixel and 126 MHz serializer clocks. The two reports list the
eight permitted reset-release paths per route and preserve the same
uncommitted host-pad timing input. Each local image is paired with its
checksum, device and frame-CRC check, and
[functional evidence](../docs/hardware/tang20k-paired-rectangles-functional.json).

The published image remains the earlier release. External electrical
margins and physical speed are unmeasured, encrypted netlists prevent the
fixed SiP check, and V1.9.8.11 installation provenance remains unverified.

### Direct texture-refill source snapshot (2026-11-25)

[Gowin V1.9.8.11](../docs/hardware/tang20k-direct-texture-refill-v1908.json)
and [signed V1.9.11.03](../docs/hardware/tang20k-direct-texture-refill-v1911.json)
used identical complete-board sources and placement/routing 0/2.
Both routes verified all 26 package pins and passed the required
clock and host-capture gates. Each reports the eight documented
reset-release violations.

| Gowin version | Core Fmax | Host Fmax | Logic | BSRAM | Board gates |
| --- | ---: | ---: | ---: | ---: | --- |
| V1.9.8.11 | 51.386 MHz | 30.651 MHz | 20,477/20,736 | 46/46 | Pass |
| V1.9.11.03 | 52.030 MHz | 26.309 MHz | 20,069/20,736 | 46/46 | Pass |

The selected V1.9.11.03 route leaves 667 mapped logic units
and uses all 46 BSRAM banks. The operating clocks stay at 50.4 MHz
core/SDRAM, 25.2 MHz pixel and 126 MHz serialization. Both reports
preserve the uncommitted host-pad constraint input. Each qualifying
local image has a matching checksum, device and frame-CRC check, and
[functional report](../docs/hardware/tang20k-direct-texture-refill-functional.json).
The published image belongs to the earlier release. Encrypted netlists
prevent the fixed SiP check, the V1.9.8.11 installation provenance remains
unverified, and external electrical margins remain unmeasured.

### Batched triangle-offset source snapshot (2026-11-25)

[Gowin V1.9.8.11](../docs/hardware/tang20k-batched-offsets-v1908.json)
and [signed V1.9.11.03](../docs/hardware/tang20k-batched-offsets-v1911.json)
used identical complete-board sources and placement/routing 0/2.
The signed route verified all 26 package pins and passed the required
clock and host-capture gates. Its timing report lists eight permitted
reset-release paths. V1.9.8.11 failed placement with 44 equivalent LUTs
unplaced. Its reported resource count is from that failed attempt.

| Gowin version | Core Fmax | Host Fmax | Logic | BSRAM | Board gates |
| --- | ---: | ---: | ---: | ---: | --- |
| V1.9.8.11 | Unavailable | Unavailable | 20,651/20,736 | Unavailable | Fail |
| V1.9.11.03 | 51.164 MHz | 26.967 MHz | 20,291/20,736 | 46/46 | Pass |

The selected V1.9.11.03 route uses 20,291 logic units and all 46 BSRAM
banks. The preceding signed route used 20,069 logic units and estimated
52.030 MHz core Fmax. The operating clocks stay at 50.4 MHz
core/SDRAM, 25.2 MHz pixel and 126 MHz serialization. Both reports
preserve the uncommitted host-pad constraint input. Each qualifying
local image has a matching checksum, device and frame-CRC check, and
[functional report](../docs/hardware/tang20k-batched-offsets-functional.json).
The published image belongs to the earlier release. Encrypted netlists
prevent the fixed SiP check, the V1.9.8.11 installation provenance remains
unverified, and external electrical margins remain unmeasured.

### Uncovered triangle-span source snapshot (2026-11-25)

[Gowin V1.9.8.11](../docs/hardware/tang20k-uncovered-batch-v1908.json)
and [signed V1.9.11.03](../docs/hardware/tang20k-uncovered-batch-v1911.json)
used identical complete-board sources and placement/routing 0/2.
The selected route passed the core, pixel and host-capture timing gates
with 26 verified package pins. The reports list any accepted timing
exceptions and failures.

| Gowin version | Core Fmax | Host Fmax | Logic | BSRAM | Board gates |
| --- | ---: | ---: | ---: | ---: | --- |
| V1.9.8.11 | 51.438 MHz | 29.881 MHz | 20,336/20,736 | 46/46 | Pass |
| V1.9.11.03 | 53.144 MHz | 28.095 MHz | 20,121/20,736 | 46/46 | Pass |

The current design operates at 50.4 MHz core/SDRAM, 25.2 MHz pixels and
126 MHz serialization. The routes retain the uncommitted 4 ns host-pad
constraint input. The matching [functional report](../docs/hardware/tang20k-uncovered-batch-functional.json)
covers the wrapper and board-clock RTL. The selected image, checksum and
frame-CRC evidence are retained locally. The published image belongs to
the earlier release. Encrypted netlists prevent the fixed SiP check.
The V1.9.8.11 installer provenance remains unverified and external
electrical margins remain unmeasured.

### Tile reference-burst source snapshot (2026-11-25)

[Gowin V1.9.8.11](../docs/hardware/tang20k-reference-burst-v1908.json)
and [signed V1.9.11.03](../docs/hardware/tang20k-reference-burst-v1911.json)
used identical complete-board sources and placement/routing 0/2.
The V1.9.8.11 route verified all 26 package pins and passed the core,
pixel and host-capture timing gates with eight documented reset-release
paths. V1.9.11.03 failed placement with 35 equivalent LUTs unplaced.
The board reports record the timing violations and exact local inputs.

| Gowin version | Core Fmax | Host Fmax | Logic | BSRAM | Board gates |
| --- | ---: | ---: | ---: | ---: | --- |
| V1.9.8.11 | 59.408 MHz | 30.513 MHz | 19,087/20,736 | 46/46 | Pass |
| V1.9.11.03 | Unavailable | Unavailable | 20,246/20,736 | Unavailable | Fail |

The design still runs at 50.4 MHz core/SDRAM, 25.2 MHz pixels and 126 MHz
serialization. Both builds used the existing uncommitted 4 ns host-pad
allowances. The selected local image has a matching checksum, structural
check and [functional evidence](../docs/hardware/tang20k-reference-burst-functional.json).
The repository programming file belongs to the earlier release. Encryption
prevents the netlist check of fixed SDRAM SiP connections. The V1.9.8.11
toolchain provenance and external electrical margins remain unverified.

### COPY write-overlap source snapshot (2026-11-25)

[Gowin V1.9.8.11](../docs/hardware/tang20k-copy-write-overlap-v1908.json)
and [signed V1.9.11.03](../docs/hardware/tang20k-copy-write-overlap-v1911.json)
used identical complete-board sources and placement/routing 0/2.
The selected route verified all 26 package pins and passed the core,
pixel and host-capture timing gates. Both reports retain the exact build
inputs and show any placement or timing failures and accepted exceptions.
The V1.9.8.11 route failed core timing at 41.817 MHz with 385 unexpected
listed timing paths. The signed route lists the eight documented
reset-release exceptions.

| Gowin version | Core Fmax | Host Fmax | Logic | BSRAM | Board gates |
| --- | ---: | ---: | ---: | ---: | --- |
| V1.9.8.11 | 41.817 MHz | 29.582 MHz | 20,263/20,736 | Unavailable | Fail |
| V1.9.11.03 | 53.362 MHz | 27.562 MHz | 20,171/20,736 | 46/46 | Pass |

The design runs at 50.4 MHz core/SDRAM, 25.2 MHz pixels and 126 MHz
serialization. Both builds used the existing uncommitted 4 ns host-pad
allowances. The selected local image has a matching checksum, frame-CRC
check and [functional report](../docs/hardware/tang20k-copy-write-overlap-functional.json).
The repository programming file belongs to the earlier release. The Gowin
netlists are encrypted, so the fixed SDRAM SiP wiring check remains
unavailable. V1.9.8.11 provenance and external electrical margins remain
unverified.

### Aligned D24 row source snapshot (2026-11-25)

[Gowin V1.9.8.11](../docs/hardware/tang20k-depth-row-v1908.json)
and [signed V1.9.11.03](../docs/hardware/tang20k-depth-row-v1911.json)
used identical complete-board sources and placement/routing 0/2.
The selected route verified 26 package pins and passed the core, pixel,
and host-capture timing gates. Each report lists the timing paths under
the existing exceptions and records any failed gates.
Both routes passed and listed eight reset-release exceptions.

| Gowin version | Core Fmax | Host Fmax | Logic | BSRAM | Board gates |
| --- | ---: | ---: | ---: | ---: | --- |
| V1.9.8.11 | 57.368 MHz | 35.266 MHz | 18,856/20,736 | 46/46 | Pass |
| V1.9.11.03 | 55.424 MHz | 27.253 MHz | 18,787/20,736 | 46/46 | Pass |

Operating clocks stay at 50.4 MHz core/SDRAM, 25.2 MHz pixels and 126 MHz
serialization. Both builds retained the existing uncommitted 4 ns host
pad constraints. The local selected image has a matching checksum,
frame-CRC check and [functional report](../docs/hardware/tang20k-depth-row-functional.json).
The committed programming image belongs to an earlier release. Encrypted
netlists prevent fixed SDRAM SiP verification. V1.9.8.11 provenance and
external electrical margins remain unverified.

### Radix-four kind-4 setup source snapshot (2026-11-25)

[Gowin V1.9.8.11](../docs/hardware/tang20k-kind4-radix-four-v1908.json)
and [signed V1.9.11.03](../docs/hardware/tang20k-kind4-radix-four-v1911.json)
built the same complete-board inputs with placement/routing 0/2.
The selected V1.9.8.11 route verified 26 package pins and passed
the core, pixel and host-capture timing gates. Both routes passed and each report lists 8 reset-release paths under the existing exceptions.

| Gowin version | Core Fmax | Host Fmax | Logic | BSRAM | Board gates |
| --- | ---: | ---: | ---: | ---: | --- |
| V1.9.8.11 | 56.037 MHz | 32.274 MHz | 19,115/20,736 | 46/46 | Pass |
| V1.9.11.03 | 50.487 MHz | 25.866 MHz | 20,391/20,736 | 46/46 | Pass |

Operating clocks remain 50.4 MHz core/SDRAM, 25.2 MHz pixels and 126 MHz
serialization. Both builds used the existing uncommitted 4 ns host-pad
constraints. The local image has a matching checksum, frame-CRC check
and [functional report](../docs/hardware/tang20k-kind4-radix-four-functional.json).
The repository programming image describes an earlier release. Encrypted
netlists prevent fixed SDRAM SiP verification. V1.9.8.11 provenance and
external electrical margins remain unverified.

## 13. Simulator, reference and performance evidence

| Implementation | Role |
| --- | --- |
| Canonical C++ SRR, monolithic/tiled modes | Independent rendering oracle |
| C++ block and timed-system models | Earlier architectural models and experiments |
| SystemVerilog compiled by Verilator | Actual current logical GPU execution |
| C++ external SDRAM model | External chip responding to physical command/address/DQ/DQM pins |
| SDL2 / PPM capture | Presentation of pixels emitted by RTL scanout |

The RTL executable links CPU setup/encoding, the external chip and the oracle;
it does not execute the C++ GPU device backends. The optimized C++ system model's
approximately 32.4 ms showcase result is not the current RTL frame time. That
older model also derives host timing from its modeled GPU clock; actual RTL
uses the independent host and core clock domains described above.

`--verify` checks both external color buffers and every D24 storage word, then
checks **every completed displayed frame after the first PRESENT**, including
frames consumed during the next upload/render. Per-frame CSV logging preserves
blackouts rather than hiding them. The scanout-starvation regression fails the
old RTL and passes the corrected queued-refill/staged-upload implementation.

The historical 0.2 default-profile RTL passed the command/recovery self-test and two
rendered frames each of showcase, perspective, overdraw and opaque, with zero
scanout underflows and matching attachment/display pixels. Showcase's second
submission is **8,719,000 core cycles**, about **86.50 ms** at the assumed
100.8 MHz. Across the four scenes, the sole encoded upload path reduces
serialized request bytes by **22.9–41.8%**, while submission cycles increase
by **0.4–3.7%** relative to the previous 0.1 timing baseline. This improves bus
capacity, not measured FPS. The local warm fragment initiation interval
remains one. These measurements include host delivery through the matching
fence and exclude RP2350 execution and a board frequency guarantee. SDL
wall-clock playback is slower because the PC evaluates every simulated clock.
The routed core misses 100.8 MHz, so those nominal milliseconds are not an
achieved FPGA frame time.
Full source-matched results, including 29 additional COPY oracle cases and 17
descriptor/LOAD fault cases, seven malformed encoded-upload cases and obsolete
version rejection, are in
[the unified-upload regression](../docs/benchmarks/rtl-unified-upload.json).
The [0.1 timing-optimization regression](../docs/benchmarks/rtl-timing-optimized.json)
retains the previous raw-upload measurements.
The earlier [area-optimization baseline](../docs/benchmarks/rtl-fit-optimized.json)
retains the pre-retiming cycle counts.
The earlier [continuous-scanout baseline](../docs/benchmarks/rtl-scanout-fixed.json)
retains the original blackout regression and pre-area-optimization counts.

### Tile attachments and SDRAM pipelining (current board profile)

`nano-gpu-rtl --benchmark` times passes and a copy on the board-profile core
(50.4 MHz core, 25.2 MHz pixel and virtual host, refresh interval 787) and
compares every case's color buffers and D24 words with the SRR. Times run from
the first host byte through the fence reply, so they include delivery of the
descriptor, 2,400-byte tile table, lists and records. *Original* is the
per-pixel LOAD design, in which every pass also loaded and stored all 300
tiles. *Bursts* adds burst tile transfers, empty-tile skipping and depth
DISCARD. *Now* adds pipelined SDRAM column commands with overlapped write
staging, the word-wide display FIFO and tile clearing during stores.

| Case | Original (ms) | Bursts (ms) | Now (ms) |
| --- | ---: | ---: | ---: |
| Empty pass, clear color and depth (first after boot) | 93.15 | 38.87 | 18.30 |
| Empty pass, clear color and depth | 105.41 | 53.59 | 23.56 |
| Empty pass, load color, clear depth | 324.73 | 35.24 | 15.59 |
| Empty pass, load color and depth | 580.62 | 0.44 | 0.33 |
| Full-screen front-to-back copy | 28.61 | 28.38 | 14.85 |
| 148-rectangle UI frame, clear color and depth | 140.82 | 98.75 | 55.03 |
| 6×9 update, load color and depth | 580.68 | 0.88 | 0.44 |
| Empty pass, clear color, discard depth | — | 18.82 | 8.23 |
| 148-rectangle UI frame, clear color, discard depth | — | 63.63 | 39.99 |
| 6×9 update, load color, discard depth | — | 0.68 | 0.48 |
| Showcase scene, second verified frame | 196.40 | 154.43 | 100.09 |
| Perspective scene | 176.57 | 134.62 | 84.55 |
| Overdraw scene | 219.53 | 178.20 | 127.66 |
| Opaque scene | 215.35 | 170.73 | 124.47 |

DISCARD did not exist in the original design. The scene rows are each scene's
second `--verify` submission; all checked display frames match the SRR with
zero scanout underflows. A `--profile` run attributes the remaining time: in
the 2D UI frame, covered rectangle pixels (one per clock) take about 40% and
record fetches about 13%; in the showcase scene, one-at-a-time texture-cache
refills stall the rasterizer for about 19% and uncovered triangle
bounding-box samples take about 6%; in the overdraw scene, uncovered samples
take about 15% and the plane offset walk about 9%. Results are in
[the SDRAM-pipeline regression](../docs/benchmarks/rtl-sdram-pipeline.json);
[the attachment-burst regression](../docs/benchmarks/rtl-attachment-bursts.json)
retains the previous step. These are simulation measurements, not observed
FPGA frame rates.

The benchmark also renders the `demo_3d_engine6` icosahedron (nine visible
Gouraud, depth-tested triangles, depth discarded). A frame with a full color
clear takes **12.71 ms**, about 70% of it writing all 300 tiles back to
SDRAM; the covered pixels take about 0.8 ms. The next pose drawn into the same
buffer with `SKIP`, listing only the tiles drawn now and those drawn by the
earlier frame (an empty-scissor marker for the latter), takes **6.83 ms** and
matches the full-clear SRR image exactly. Results are in
[the SKIP regression](../docs/benchmarks/rtl-skip-tiles.json).

**Kind-4 vertex triangles.** The benchmark also submits kind-4 records
directly. Each of two runs has 400 random triangles: large, small, sliver
and zero-area shapes in both windings, a third with the cull bit, a quarter
with a scissor rectangle, and positions in the guard band. The first run
lists each triangle in its bounds tiles; the second widens every list by one
tile, so the setup unit must reject windows and uncovered tiles itself. A
flat icosahedron also submits every face twice, the second copy reversed and
culled. Every color buffer and D24 word matches the SRR's decode of the same
records. The icosahedron takes **11.84 ms**, against 12.71 ms for the
CPU-prepared Gouraud frame. Results are in
[the vertex-triangle regression](../docs/benchmarks/rtl-vertex-triangles.json).

A board-clock replay of `demo_3d_engine9`'s town view reproduces the same
640×480 image (scanout hash `a15aa053fd7b1558`) as with CPU-prepared records.
The trace was recorded from the NARM program running under an emulator.
Encoded uploads drop from 37.3 KB to 28.4 KB per frame, and host delivery
from 449k to 182k core clocks. The GPU pass stays at 1.42 M clocks (28.2 ms,
against 1.43 M): the setup unit's 9% replaces the four extra record bursts and
the offset walk. Tile stores (about 32%), covered pixels (23%) and uncovered
bounding-box samples (12%) dominate the pass. In the same emulator, the
program's per-frame triangle building falls from about 3.7 M to 2.4 M
instructions, and its binning from 1.9 M to 0.6 M. These are simulation and
emulation figures, not board measurements.

**Board measurement, 2026-11-10.** The user ran `demo_3d_engine9`'s timing
build on the board, at its starting view (about 445 records, 1,660 tile
references, one pass), with the previous image (`93739598…f886`, before
early SDRAM release) and an RP2350 at 375 MHz. The measured
GPU render time is **30.2 ms**, consistent with the 28.2 ms replay above. It
stays about 24–31.5 ms in other views: a sphere at 20 or 1,280 Gouraud faces
measured 24.0 or 31.5 ms, so the pass is dominated by its fixed per-frame
tile work, about 6 µs per added triangle. The host side was reduced in
steps, each measured: 9 FPS for the original NARM program, then 15, then 20.
- MPC-2350 firmware services and the program's per-frame code were placed in
  on-chip SRAM.
- The program's Gouraud and texture plane setup changed to binary32, with
  exact 64-bit cross products and an exact fallback for ill-conditioned
  slivers. Measured against the exact setup on the PC: at most 9 D24 codes,
  one 8-bit colour step and 0.018 texel.
- Frames were pipelined: the CPU builds and bins frame N+1 while the GPU
  renders frame N, using the blocking PRESENT.
- Uploads were moved to DMA, then to write bursts (control guide).
The 20 FPS frame is now about 4.6 ms of uploads, 30.2 ms of rendering and the
wait for the third vertical blank. Uploads cannot overlap rendering because
the command queue executes in order. 30 FPS needs uploads plus rendering
below 33.3 ms. These are user-reported on-screen timings, not a qualified
benchmark. Like MC10's, the binary32 setup is not bit-identical to the oracle.

### Early SDRAM release and overlapped tile stores

A `--profile` run of the previous RTL showed tile stores dominated by
per-transaction overhead rather than data beats: each 16-word transfer waited
for the previous transaction's completion before its grant, check and row
activation. Scanout refills took about 40% of SDRAM time during stores. The
current RTL releases the command slot at the last column command, keeps two
transactions and two scanout refills in flight, and issues tile stores back
to back (section 9). The same oracle-checked benchmark, before and after:

| Case | Before (ms) | Now (ms) |
| --- | ---: | ---: |
| Empty pass, clear color and depth (first after boot) | 18.30 | 12.72 |
| Empty pass, clear color and depth | 23.56 | 17.97 |
| Empty pass, load color, clear depth | 15.59 | 12.32 |
| Empty pass, clear color, discard depth | 8.23 | 5.75 |
| Full-screen front-to-back copy | 14.85 | 13.78 |
| 148-rectangle UI frame, clear color and depth | 55.03 | 48.33 |
| 148-rectangle UI frame, clear color, discard depth | 39.99 | 36.57 |
| Pointer-sized update, load color and depth | 0.44 | 0.48 |
| Showcase scene frame | 99.62 | 89.26 |
| Overdraw scene frame | 127.65 | 118.75 |
| Icosahedron, full clear | 12.71 | 10.53 |
| Icosahedron, SKIP with dirty tiles | 6.83 | 6.22 |
| Icosahedron, flat kind-4 with GPU culling | 11.84 | 10.09 |
| 400 kind-4 triangles, 12,950 references | 159.66 | 148.84 |
| 400 kind-4 triangles, 17,541 references | 152.56 | 141.04 |

Every case matches the SRR's color and D24 words, with zero scanout
underflows. The small pointer update is 0.04 ms slower: its single tile now
pays the end-of-pass drain separately. Full results are in
[the early-release regression](../docs/benchmarks/rtl-sdram-early-release.json).
Scenes gain 7–17% and clear-dominated passes up to 30%. The replay harness
for `demo_3d_engine9`'s town view was not kept, so its new pass time is not
measured. From the store share of its pass (about 32%), a gain similar to the
scenes, roughly 2–3 ms of the 30.2 ms measured on the board, is an estimate
only. These are simulation figures, not board measurements.

**Board measurement, 2026-11-12 (image `ecc7030b…608c`).** At the same
starting view, the user measured 30 fps. The rows of the pipelined timing
build add up to a 33.0 ms frame: UPD 1.3, CPU 12.5, BIN 3.6, a 10.6 ms wait
for rendering, 0.4 ms VBL and 4.6 ms of uploads. Its label read GPU on that
copy; it is the wait row. Rendering is therefore about 28.0 ms, down from
30.2 ms, in line with the 2–3 ms estimate. The frame is 0.3 ms inside the
30 fps period. Close to the car, the frame dropped to 15 fps:
- 4,025 tile references from 335 records, against 1,664;
- a 33.9 ms wait, so about 49.4 ms of rendering;
- 12.2 ms of vertical-blank wait and 4.5 ms of uploads.

Large facets cross many tiles, so each tile holds about 13 references, each
walking its window. These are user-reported on-screen timings.

### Row exit, CPU culling and the close-up case (before pipelined edge setup)

A close-up case added to the benchmark draws a coarse sphere that fills most
of the screen, in front of a finer one that it partly hides. Both are
kind-4 flat triangles whose back faces the CPU drops before binning, as the
draw SDK now does. With row exit (section 6), the same oracle-checked
benchmark gives:

| Case | Before (ms) | Now (ms) | Empty samples before | Empty samples now |
| --- | ---: | ---: | ---: | ---: |
| Showcase scene frame | 89.26 | 86.32 | 307,452 | 143,780 |
| Overdraw scene frame | 118.75 | 115.85 | 1,004,673 | 844,876 |
| Icosahedron, full clear | 10.53 | 10.00 | 63,965 | 32,451 |
| Icosahedron, SKIP with dirty tiles | 6.22 | 5.54 | 68,022 | 36,218 |
| Icosahedron, flat kind-4 with GPU culling | 10.09 | 9.77 | 39,065 | 21,907 |
| 400 kind-4 triangles, 12,950 references | 148.84 | 134.54 | 1,655,694 | 902,470 |
| 400 kind-4 triangles, 17,541 references | 141.04 | 128.52 | 1,458,196 | 805,595 |
| Close-up spheres, 144 records, 1,171 references | 23.95 | 22.04 | 222,166 | 120,563 |

Rectangle, copy and clear cases are unchanged. Every case matches the SRR,
with zero scanout underflows; the self-test and the four verified scenes also
pass. Results are in [the row-exit regression](../docs/benchmarks/rtl-row-exit.json).
The 2026-11-13 timing fixes (section 12) leave every cycle count of this
benchmark unchanged.
In the close-up case, the rasterizer runs only 32% of the pass. Tile stores
take about 39%. The kind-4 setup takes about 107 clocks per reference (11%),
and this RTL does not overlap it with the previous reference's drawing.
Removing back-face references on the CPU removes both the setup and the
record fetch for those references.

The [earlier board-profile regression](https://github.com/apc53pro/Nano-20K-GPU/blob/e538741/docs/benchmarks/rtl-tang20k.json) ran the real core at the 50.4 MHz
core / 25.2 MHz host and pixel ratios, with refresh interval 787. Command tests
and two frames of all four scenes pass; every checked display frame and
RGB565/D24 attachment matches the SRR, with zero scanout underflows. Its last
submission measurements include host delivery through the matching fence:

| Scene | Core cycles | Virtual time at 50.4 MHz (ms) |
| --- | ---: | ---: |
| showcase | 9,853,572 | 195.51 |
| perspective | 8,912,684 | 176.84 |
| overdraw | 11,052,888 | 219.30 |
| opaque | 10,736,252 | 213.02 |

These are historical simulation measurements, not observed FPGA FPS. They
predate the burst copy engine. They exclude CPU
transform/setup work and presentation wait. The board profile is a first
hardware bring-up target, not a claim that complete scenes reach 30 FPS.
Warm resident-tile one-fragment-per-clock cadence remains verified separately.
The early read-response streaming change is required for zero scanout
underflow at this two-core-clocks-per-pixel profile.

### Pipelined edge initialization (2026-11-24)

[The new experiment](../docs/benchmarks/rtl-edge-setup-pipeline.json) records
fresh before/after board-core RTL simulations at the unchanged 50.4 MHz core
and 25.2 MHz virtual host/pixel clocks. Every case checks both complete RGB565
buffers and every depth word against the SRR; scanout underflows remain zero.
The clear, rectangle, pointer and copy cases are cycle-identical. Every fence
in the continuous benchmark sequence completes no later than before, but
individual pass durations are sensitive to the changed scanout/SDRAM phase:
full-clear icosahedron increases from 503,830 to 515,676 clocks and close-up
spheres from 1,110,726 to 1,112,108. These results remain in the report.

To separate those changed arrival phases from the work itself, additional
measurements start each workload as the first submission of a freshly booted
simulation, at identical idle offsets, with normal packet uploads and fences.
For the four tested offsets, full-clear icosahedron takes 0.58–0.63% fewer
clocks and close-up spheres 1.18–1.38% fewer. This is scoped simulation
evidence, not a guarantee that every arrival phase improves, and not physical
board FPS. The report includes the measurement driver and input hashes.
Across all 20 matched-start pairs (eight workloads), clock counts decrease
by 0.16–1.38%; the full continuous sequence uses 0.65% fewer clocks.

The supporting whole-board reports identify the exact local build inputs,
including the same separately owned, uncommitted host-pad constraints as the
retained image. A clean checkout alone therefore does not reproduce those
routes. The local candidate images stay beside their checksums and reports;
the published programming-image bundle is unchanged. Neither image was
programmed or measured on hardware by this experiment.

### Kind-4 depth-divider prefix (2026-11-24)

The [divider measurements](../docs/benchmarks/rtl-divider-prefix.json) compare
the previous and current RTL with the same 50.4 MHz core, 25.2 MHz virtual
host/pixel clocks and normal packet delivery through the fence. Both RGB565
buffers and every depth word match the independent renderer. Scanout records
zero underflows. The focused test checks signed gradients, the strict prefix
threshold, rounding, saturation, reverse winding and reset across 40 records.

Twelve executions of four kind-4 workloads start after fresh boots at identical
idle offsets. The 400-triangle submissions use 0.96–1.31% fewer clocks, the
spheres 1.41–1.60% fewer, and the flat icosahedron 0.33–0.60% fewer.
The continuous suite takes 32,967,060 clocks, down from 33,122,732 (0.47%).
Every cumulative fence is at least as early. The flat icosahedron alone lasts
1.87% longer in that sequence because the prior passes shift its arrival
phase. The profile shows more tile-store command wait and fewer setup clocks
for that pass. The report retains that duration alongside the matched-start
comparisons. These times measure simulated submission and completion at the
board clock.

### Paired tile attachment transfers (2026-11-24)

The [paired-transfer benchmark](../docs/benchmarks/rtl-paired-attachments.json)
compares Verilated board-core runs at 50.4 MHz, including normal host uploads
through the fence. Both RGB565 buffers and every D24 word match the
independent reference renderer. Scanout reports zero underflows. In 30
matched-start runs across 15 workloads, every case improves or ties.

| Workload | Cycle reduction at matched starts |
| --- | ---: |
| UI, storing depth | 7.81–7.89% |
| UI, discarding depth | 11.90–12.21% |
| UI, loading color and depth | 10.55–10.73% |
| Showcase and perspective | 4.87–5.52% |
| Overdraw and opaque | 2.88–3.23% |
| Close-up spheres | 14.62–15.09% |
| Full-screen copy and empty constant clear | 0% |

The continuous suite takes 31,482,680 clocks, down from 32,967,060
(4.50%). The dirty-tile icosahedron pass uses 16.77% fewer clocks in
that sequence. Every cumulative fence completes at least as early.
The empty color-clear pass with depth discarded takes 10.35% longer
in the continuous sequence, where earlier passes move its start to a
different scanout and SDRAM phase. Its two matched-start measurements
are cycle-identical across the change. Both timings are recorded.

Focused tests check all attachment addresses, both write parities, output
hold, reset retention and clear-back while reading the next pair. The
wrapper's partial writes were also checked against both installed Gowin
GW2A primitive models.

### Short 2D primitive records (2026-11-24)

The [short-record benchmark](../docs/benchmarks/rtl-short-2d-records.json)
compares the previous packet and record versions with 0.3 using Verilated
board-core RTL. Each submission includes encoded heap uploads and runs
through its fence at a 50.4 MHz core clock and a 25.2 MHz virtual host. Both
complete RGB565 buffers and full D24 storage match the independent renderer,
with zero scanout underflows. The continuous suite falls from 31,482,680 to
30,295,388 core clocks, a **3.77% reduction**.

| Workload | Cycle reduction at matched starts |
| --- | ---: |
| UI, storing depth | 13.76–15.09% |
| UI, discarding depth | 17.58–18.75% |
| UI, loading color and depth | 8.44–9.54% |
| Showcase and perspective | 7.89–8.95% |
| Overdraw | 2.87–3.67% |
| Full-screen copy, opaque and triangle-only controls | 0% |

The 148-rectangle UI with depth store submits 12,386 bytes instead of
15,503, including record, table, list, command and fence bytes. Textured
scenes still include their first-use uploads. Across 36 matched starts,
each case improves or ties. Five individual durations in the continuous
sequence increase by 0.09–2.76% as preceding passes shift scanout and SDRAM
phase. Every cumulative fence is at least as early. Matched empty LOAD,
dirty SKIP after an identical prior frame, and prepared or vertex triangle
runs isolate those changes. The evidence records both sets of durations
and logical-host controls at 18.75 MHz and near 250 kHz. At
18.75 MHz the matched UI submissions use 15.07% fewer core clocks with
depth store and 18.82% fewer with depth discard. The simulated host clocks
represent nominal burst rates; they exclude RP2350 software and PIO
overhead.

Focused RTL regressions check exact 2D tail reads at the heap boundary,
faults before an out-of-bounds tail request, and cancellation of staged
record words. They also draw using short rectangle, line and keyed sprite
records after a prepared triangle and reset, checking complete tile RGB565
and D24 contents and preventing stale coefficients from affecting 2D draws.

### Paired rectangle rasterization (2026-11-25)

The [paired-rectangle benchmark](../docs/benchmarks/rtl-paired-rectangles.json)
compares the same 0.3 packet bytes through the fence on Verilated board-core
RTL at 50.4 MHz, with a 25.2 MHz virtual host. Both RGB565 buffers and every
D24 word match the independent SRR, with zero scanout underflows.

| Workload | Cycle reduction at matched starts |
| --- | ---: |
| UI, storing depth | 20.35–23.55% |
| UI, discarding depth | 30.21–33.91% |
| UI, loading color and depth | 13.65–14.95% |
| Mixed depth and odd scissor boundaries | 0.88–1.15% |
| Showcase and perspective | 3.39–4.16% |
| Overdraw | 2.24–2.86% |
| Opaque, full-screen copy, empty passes and triangle-only controls | 0% |

Each 148-rectangle UI submission covers 797,440 pixels. The aligned pairs
cut occupied raster clocks from 797,440 to 399,232, including 1,024
singleton pixels. Each matched submission used the same packet bytes,
including first-use resource uploads in textured scenes. The continuous
suite uses 29,215,610 clocks instead of 30,295,388, a **3.56% reduction**.
Every cumulative fence is at least as early. Four individual pass durations
increase in the continuous sequence as earlier completions shift scanout
and SDRAM phase. The two pointer updates reproduce those increases at
their original start times. Their occupied raster clocks still fall from
54 to 27. Four controlled pointer starts with identical attachments and
the same packet bytes within each pair tie or improve. All 38 other
matched-start cases also tie or improve.

Focused regressions check both start parities and clipped widths, every
D24 comparison and both attachment masks, including different outcomes
for the two lanes. They also check viewport edges, final row retirement,
reset and fault suppression. The exact per-sample coordinate and overflow
traces still cover the triangle walker.

### Direct texture-word refills (2026-11-25)

The [direct-refill benchmark](../docs/benchmarks/rtl-direct-texture-refills.json)
uses the same N20K-SYS-0.3 packets and 50.4 MHz board-core clock. Both
complete RGB565 buffers and every D24 word match the independent SRR,
and scanout reports zero underflows.

Showcase keeps the same 26,949 demand refills and 215,592 accepted SDRAM
words. The data bank uses 215,592 write clocks instead of 431,184. On
this workload the mean first-to-last response span falls from 14 to 7
clocks, and the mean last-response-to-tag interval falls from 2 to 1.
The texture input now accepts every available response word, removing
188,643 response clocks where the cache previously asserted backpressure.
Raster texture stalls fall from 851,456 to 620,735 clocks.

The continuous showcase pass takes 3,409,962 instead of 3,671,750 clocks
(7.13% fewer). Overdraw takes 5,258,854 instead of 5,293,356 clocks
(0.65% fewer). The continuous suite falls from 29,215,610 to 28,962,680
clocks (0.87%). Every cumulative fence is at least as early. Four later
untextured pass durations rise in the continuous sequence as earlier
completions shift scanout and SDRAM phase. The benchmark records these
durations and the matched-start controls. All 46 matched starts improve or
tie. Showcase uses 6.73–7.02% fewer clocks, perspective 4.70–4.96% fewer,
and overdraw 0.697–0.701% fewer. Untextured controls have the same cycle
count at matched starts.

The refill-heavy repeat sampler uses 33,280 texture upload bytes. Its
matched end-to-end time falls by 1.97–3.24% while those first-use bytes
remain part of the measurement. With the same asset seeded and resident,
the pass uses 2.46–3.70% fewer clocks. The seed is measured separately and
is identical before and after. Warm-hit and pitched keyed-sprite controls
also improve or tie. The pitched case exercises bases offset from a
32-byte burst boundary and one-, two- and four-word final lines.

Focused checks cover all eight word counts for a short final line, all
eight four-byte base offsets within an SDRAM burst, consecutive and
bubbled responses, exact padding, malformed completion and reset.
Warm hits retain seven-edge latency and 1,024 consecutive outputs at
one output per clock.

### Batched triangle offsets (2026-11-25)

The [batched-offset benchmark](../docs/benchmarks/rtl-batched-triangle-offsets.json)
compares the same 0.3 packets on Verilated board-core RTL at 50.4 MHz.
Both complete RGB565 buffers and every D24 word match the independent
SRR, with zero scanout underflows.

In continuous overdraw, the X/Y offset walk falls from 622,711 to 160,768
clocks. This matches the count predicted from every starting axis length,
including the single-step tails. That pass uses 4,796,164 instead of
5,258,854 clocks through its fence (8.80% fewer). Showcase's walk falls
from 127,503 to 32,823 clocks and its pass improves 2.90%. The continuous
suite falls from 28,962,680 to 28,353,194 clocks (2.10%). Every cumulative
fence is at least as early. One kind-4 pass in the continuous sequence
takes 150 additional clocks as its start phase changes. Its matched-start
duration stays identical.

| Workload | Cycle reduction at matched starts |
| --- | ---: |
| Opaque | 16.40–16.80% |
| Overdraw | 8.92–8.98% |
| Showcase and perspective | 2.65–2.85% |
| Prepared full-clear icosahedron | 2.19–3.24% |
| Prepared icosahedron with SKIP and identical prior attachments | 5.07–6.43% |

All 48 matched starts improve or tie. Timing for the original two-frame
SKIP sequence is reported separately because the first frame also
changes duration. The controlled SKIP run recreates the first frame's
full attachments with exact tile-local seeds and matches the second
frame's starting phase.

The prepared wide-triangle workload includes the same 576 first-use
texture bytes and the same covered raster samples. It uses 16.56–17.37%
fewer clocks at the two matched starts. The focused RTL regression
checks positive and negative slopes, the signed 33-bit batching limits
and adjacent wider values, clipping at every low five-bit axis residue,
wide signed-boundary cancellation, masked planes, axis ordering, and
actual RGB565, D24 and texture coordinates. It also checks that overflow
on either axis faults before any attachment write.

### Uncovered triangle spans (2026-11-25)

The continuous board-core RTL suite runs in 27,253,874 clocks at 50.4 MHz,
down from 28,353,194 clocks (3.88%). All cumulative fences are at least as
early. Overdraw falls from 4,796,164 to 4,397,990 clocks (8.30%). Its
130,161 five-sample groups eliminate 390,483 raster clocks, with all
ordinary row exits and covered samples preserved. The two 400-triangle
kind-4 passes use 4.87% and 4.62% fewer clocks. In the continuous run two
icosahedron passes start at different video and memory phases and take
4,548 and 2,572 more clocks individually. Both improve at matched starts.

| Workload | Cycle reduction at two matched starts |
| --- | ---: |
| Opaque | 19.38–20.21% |
| Overdraw | 7.96–8.22% |
| Showcase | 1.64–1.79% |
| Perspective | 2.16–2.27% |
| Kind-4 close-up spheres | 5.85–6.47% |
| Prepared icosahedron with SKIP and identical prior attachments | 7.06–9.73% |
| Sparse textured triangles with bounded D24 slopes | 14.55–16.90% |

All 50 main matched starts and two focused sparse starts improve or tie,
with pixel-exact RGB565 and full D24 equivalence to the independent SRR
and zero scanout underflows. The sparse tests include 576 bytes of
first-use texture uploads. The original wide-slope sparse control ties
because its active depth coefficients keep it on the normal scalar walk.
Both natural SKIP sequences also improve when the first and second frame
are timed together. See the [RTL cycle evidence](../docs/benchmarks/rtl-uncovered-triangle-batch.json)
for per-workload counts and the matched state. These are simulator
submission-to-fence results at the existing operating clock.

### Batched tile-reference reads (2026-11-25)

The continuous [Verilated board-core benchmark](../docs/benchmarks/rtl-tile-reference-bursts.json)
falls from 27,253,874 to 26,587,478 clocks through its fences (2.45%) at
50.4 MHz. All cumulative fences are at least as early. In the two
400-triangle kind-4 passes, 12,950 and 17,541 individual reads become
950 and 1,235 logical requests. Their SDRAM BL8 read commands fall
to 2,474 and 3,283, respectively. Both passes execute all their original
references and take 3.04% and 5.22% fewer total clocks. The reference fetch
states, including buffered consumption, shrink from 255,574 and 387,417
clocks to 48,405 and 63,544.

| Workload | Cycle reduction at two matched starts |
| --- | ---: |
| Overdraw | 1.35–1.57% |
| Opaque | 2.38–2.50% |
| UI with both attachment stores | 1.46–1.66% |
| UI with discarded depth | 2.80–3.02% |
| 2,460 ordered references across 300 tiles | 2.66–2.71% |
| 300 singleton lists | Identical |

All 54 matched starts improve or tie with independent RGB565 and full
D24 equivalence and zero scanout underflows. The benchmark includes
identical first-use uploads where assets are new. In the continuous run,
the empty color clear with depth discarded and the dirty SKIP frame
take 3,530 and 6,174 more clocks individually because their start phases
shift. The separate matched-start cases and the original two-frame SKIP
sequences are recorded in the benchmark. These are RTL timings through
the command fence at the operating clock.

### Overlapped COPY writes (2026-11-25)

In the continuous [RTL benchmark](../docs/benchmarks/rtl-copy-write-overlap.json),
the full-screen front-to-back copy falls from 694,276 to 623,802 clocks
(10.15%) at 50.4 MHz. Both runs stage 153,600 words in 9,600 write requests
and execute 19,200 physical BL8 writes. The candidate grants 5,976 requests
while a prior write is still in flight. Copy drain time falls from 90,703
to 4,583 clocks. The request state can include waiting for the first write
to release the command slot, so the drain reduction exceeds the net gain.

| Workload | Cycle reduction at two matched starts |
| --- | ---: |
| Patterned full-screen front-to-back copy | 7.86–10.75% |
| 640×464 upward scroll | 10.16–10.61% |
| Odd-lane upward overlap | 5.34–7.88% |
| Clipped downward overlap | 1.34–4.36% |
| Odd-lane leftward overlap | 5.96–6.08% |
| Clipped rightward overlap | 4.82–4.85% |
| Pitched heap source, including first-use upload | 1.22–1.36% |
| Skinny and short one-burst rows | Identical |

All 20 matched COPY runs improve or tie. Independent SRR checks every RGB565
and D24 word, with zero scanout underflows. The remaining matched scenes tie.
The continuous suite takes 26,496,856 instead of 26,587,478 clocks (0.34%).
Every cumulative fence is at least as early. The continuous workload ledger
also retains the individual duration changes when the earlier COPY moves
later submissions to different video and memory phases. The pointer update
with depth discarded takes 3,216 extra clocks in that sequence. The same
pointer update ties at both matched phases, including an identical preceding
UI image and complete D24 attachment.

### Aligned D24 transfers (2026-11-25)

The [continuous Verilated benchmark](../docs/benchmarks/rtl-d24-row-transfers.json)
takes 25,946,532 instead of 26,496,856 clocks through its fences (2.08%)
at 50.4 MHz. Every cumulative fence is earlier or equal. The empty pass
that clears depth while loading color takes 554,214 instead of 621,056
clocks (10.76%). Its logical D24 writes fall from 19,200 to 9,600. All
307,200 D24 words and 38,400 physical BL8 writes are preserved. In the
pointer LOAD/STORE pass, both depth reads and writes fall from 64
requests to 32 with unchanged payload and physical burst counts.

| Workload | Cycle reduction at two matched starts |
| --- | ---: |
| Full-frame depth-only CLEAR | 9.49–10.38% |
| Full-frame color and depth CLEAR | 7.55–8.17% |
| Sparse D24 LOAD and STORE | 4.26–5.84% |
| Sparse D24 LOAD and DISCARD | 3.53–4.84% |
| UI with both attachments stored | 4.22–4.35% |
| UI with depth discarded | Identical |

The continuous suite retained zero underflows, a 96-clock maximum scanout
offer-to-grant wait, a 45-clock maximum grant-to-first-word delay and a
986-word minimum active-refill FIFO occupancy. That minimum excludes the
intentional end-of-frame drain after all requests have been issued. These
are measured bounds for these traces at the existing operating clock.
The workload ledger includes individually longer continuous passes caused
by shifted start phases. The matched starts and the natural seed-plus-pass
histories verify the complete RGB565 and D24 attachments with independent SRR.
Across 92 matched starts, every pass improves or ties. The additional
eight-case attachment matrix varies LOAD/CLEAR and STORE/DISCARD independently
for depth with mixed color load flags, and verifies final attachment validity.

### Radix-four kind-4 gradient setup (2026-11-25)

The [continuous Verilated benchmark](../docs/benchmarks/rtl-kind4-radix-four.json)
takes 25,621,650 through-fence clocks instead of 25,946,532
(1.25%) at 50.4 MHz. The four kind-4 cases issue
the same gradient divides, with 367,829 divider-busy clocks compared with
695,889 (47.14% fewer). Each setup saves exactly
17 or 21 digit clocks per nonzero axis, depending on the existing skipped prefix.

| Workload | Cycle reduction at two matched starts |
| --- | ---: |
| Dense kind-4 triangles, margin 0 | 2.74–2.92% |
| Dense kind-4 triangles, margin 1 | 2.55–2.62% |
| Flat kind-4 icosahedron | 0.93–1.18% |
| Kind-4 close-up spheres | 3.66–3.87% |
| Accepted winding, near-flat and extreme depth | 0.71–1.41% |
| Randomized depth, nonuniform starting attachments | 1.47–1.56% |

Independent SRR verified the two complete RGB565 buffers and all D24 words
at both ends of 96 matched runs, with identical packets, residency and
starting video/SDRAM phase. All 84 non-kind-4 controls tie and scanout has
zero underflows. Every continuous cumulative fence arrives earlier or
ties. The continuous flat kind-4 icosahedron takes 16,058
more clocks within its own interval after earlier passes advance the
start from near vertical blank to active scanout. The trace records
2,202 additional SDRAM reads and the same number of physical writes.
Controlled replays of both original video phases save 6,052 and 3,842
clocks. Its continuous fence arrives
291,212 clocks earlier.
These measurements cover RTL submission through fence. Displayed FPS
also depends on presentation timing and board measurements.

## 14. Build, verify and reach the board

For a Windows MSYS2 UCRT64 checkout, first complete the package upgrade described
in [development setup](../docs/development.md#windows-rtl-simulator), then:

```bash
./scripts/setup-rtl-sim.sh
make rtl-gpu-run
```

The setup checks the actual C++/Verilator/SDL toolchain. `make rtl-gpu` only
builds; `make rtl-gpu-run` also opens the window. On Linux x86_64 the same
simulator setup uses the pinned local HDL toolchain. Run verification with:

```bash
make rtl-gpu-test
make rtl-block-test
make rtl-gpu-synth
make rtl-gpu-pack
make rtl-gpu-pnr
```

The older `rtl-gpu-pack` and `rtl-gpu-pnr` targets qualify the logical-core
serial probe, not the production board target. Their nominal 100.8 MHz target
is separate from the fixed board clock plan. The retained probe reports above
are historical and must not be used as current board evidence.

Build the complete board with Gowin EDA, running the functional checks from
MSYS2 UCRT64:

```bash
make tang20k-test
make tang20k
```

`make tang20k` uses Gowin EDA V1.9.8.11 by default (see section 12).
Set `GOWIN_SH` to select another `gw_sh.exe`.

`make tang20k-test` checks wrapper clock ratios, SDRAM output phase/registers,
address/data bus behavior and the real core at the board profile. Wrapper
isolation tests use a small test-only core model; the five integration cases
use the real GPU and external SDRAM model. `make tang20k` then requires that
passing evidence. It builds the board with Gowin and checks:
- every package pin;
- the four required clocks;
- that every negative-slack path is a listed reset-release path.

It emits no `.fs` after a failed gate. Outputs are `build/tang20k/nano-gpu.fs`
and `build/tang20k/board-report.json`. Changing RTL, constraints or the builder
requires rebuilding the image and matching report; previously generated files
do not qualify a changed design. `AGENTS.md` records why the vendor flow is
used and which gate it cannot meet.

The repository programming image is `bitstreams/tangnano20k/nano-gpu.fs`, with
its release SHA-256 file and matching complete-board, functional and
programming-file reports under `docs/hardware/`. Earlier performance evidence
under `docs/benchmarks/` remains historical for the changed board profile. See the
[control guide's programming instructions](GPU_CONTROL.md#programming-and-first-board-bring-up)
for loading with Gowin Programmer, reset release and expected idle output.
The FPGA contains no command player or demo assets; a host must submit work.

**Hardware history, 2026-11-07:**
- The open-source flow's image failed on the received board. It configured,
  drove video timing and answered handshakes, but latched wrong host
  addresses. Wiring, pins, RTL and synthesis were cleared independently, and
  a pin-identical open-flow bus-echo image (`experiments/bus-echo`) worked.
  The fault lies in that flow's packing, placement or bitstream generation for
  the full design.
- The same RTL built with Gowin reached `READY`. At the then-current 6.3 MHz,
  its slow per-pixel full-screen copy outlasted the host's old 250 ms reply
  wait. Live status showed the core busy without faults.
- After that:
  - the host now polls reply availability with a 10 s deadline and stops on
    fault flags;
  - copies use SDRAM bursts;
  - the terminal no longer copies the whole screen per frame;
  - the board returned to 50.4 MHz with GPU video. The user reported the
    terminal on HDMI with the 50.4 MHz Gowin build before burst copies.

Remaining hardware work includes testing the PIO transport and native GPU
services on the board, measuring the external bus and SDRAM timing margins,
and measuring rendering performance. Gowin's static timing uses its device delay models;
external setup/hold margins and real maximum clock are not established by
Verilator or a successful bitstream generation. The board image does not claim
a measured silicon frequency limit or 30 FPS scenes.

## 15. Implementation map

| Source | Architectural responsibility |
| --- | --- |
| [`tang20k_top.sv`](../rtl/board/tang20k_top.sv) | Board clocks/reset, synchronized pixel-event handoff, shared host pads, SDRAM boundary and TMDS integration |
| [`tang20k.cst`](../rtl/board/tang20k.cst), [`tang20k-sip.json`](../rtl/board/tang20k-sip.json) | External QN88 constraints and expected fixed embedded SDRAM terminals |
| [`nano_gpu_core.sv`](../rtl/gpu/nano_gpu_core.sv) | Command execution, heap fetch, tile schedule, overlapped tile and COPY writes, overlapped presentation, recovery |
| [`nano_host_link.sv`](../rtl/gpu/nano_host_link.sv) | Host clock crossing, bounded native FIFOs, parser and packet leases |
| [`nano_triangle_setup.sv`](../rtl/gpu/nano_triangle_setup.sv) | Kind-4 vertex-triangle setup per tile: orientation/culling, edges, window and tile rejection, depth gradients and seed |
| [`nano_tile_renderer.sv`](../rtl/gpu/nano_tile_renderer.sv) | Native tile attachments, coverage with row exit and uncovered steps, interpolation and 2D walking |
| [`nano_fragment_pipeline.sv`](../rtl/gpu/nano_fragment_pipeline.sv) | Textured contexts, ordered depth/key/shading commits |
| [`uv_pipeline.v`](../rtl/uv_pipeline.v), [`uv_mult.v`](../rtl/uv_mult.v) | Selected exact integer-texel path, separate generic Q16 path, native delay RAMs and multipliers |
| [`nano_texture_cache.sv`](../rtl/gpu/nano_texture_cache.sv) | Pitched texture addressing, cache hits and refills |
| [`nano_sdram_service.sv`](../rtl/gpu/nano_sdram_service.sv) | Physical SDRAM transactions, early command release with two payload slots, timing, refresh and arbitration |
| [`nano_scanout.sv`](../rtl/gpu/nano_scanout.sv) | Independent timing, up to two in-flight refills and display FIFO consumption |
| [`nano_sdp16.sv`](../rtl/gpu/nano_sdp16.sv) | Native RAM binding and simulation port contract |
| [`nano_sdp32.sv`](../rtl/gpu/nano_sdp32.sv) | Native 512×32 coefficient banks and synchronous-port contract |
| [`nano_sdp36.sv`](../rtl/gpu/nano_sdp36.sv), [`nano_sdp8.sv`](../rtl/gpu/nano_sdp8.sv) | Native 512×36 context/delay and 2048×8 packet banks |
| [`nano_tmds.sv`](../rtl/gpu/nano_tmds.sv) | Standard TMDS encoding/serialization building block |
| [`reference/rtl/main.cpp`](../reference/rtl/main.cpp) | CPU-side demo driver, oracle comparison and actual scanout capture |
| [`reference/rtl/sdram_model.cpp`](../reference/rtl/sdram_model.cpp) | External SDRAM chip model |
| [`reference/src/setup.cpp`](../reference/src/setup.cpp) | Canonical CPU geometry/plane preparation and the kind-4 GPU setup contract |
| [`reference/src/hw_protocol.cpp`](../reference/src/hw_protocol.cpp) | CPU packet/record encoding and heap-building helpers |

For exact CPU control, start with [GPU_CONTROL.md](GPU_CONTROL.md). Supporting
historical/specification files may have a narrower scope or describe an earlier
model; they do not supersede this pair's description of the current GPU.
