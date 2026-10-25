# Tang Nano 20K hardware profile

The SRR block models target the Tang Nano 20K's **GW2AR-LV18QN88C8/I7**.
Sipeed's device-selection table identifies the architecture as **GW2AR-18C**.
Device limits below come from the sources listed at the end. Selected primitive
configurations are project choices within those limits.

The executable constants are in
[device.hpp](../../reference/include/nano_gpu/hw/device.hpp). The profile covers
documented digital behavior; vendor-library equivalence, synthesis, placement,
timing closure, and board validation remain necessary.

## Device and package limits

| Resource | Tang Nano 20K limit |
| --- | ---: |
| LUT4 | 20,736 |
| Flip-flops | 15,552 |
| BSRAM | 46 physical blocks, 18,432 bits each; 847,872 bits total |
| Shadow SRAM | 41,472 bits |
| 18×18 multiplier equivalents | 48 |
| DSP grouping | 24 macros in 12 DSP blocks, derived from the documented grouping |
| Available PLLs | 2 in QN88 |
| SDR SDRAM | 8 MiB, 32-bit data bus |

The GW2AR family maximum of four PLLs does not describe this QN88 board. BSRAM
capacity includes the extra bits available in ×9 configurations; a ×16 instance
still occupies a complete physical block. These limits do not reserve resources
for scanout, host control, or future extensions beyond the explicit allocations
below.

The complete system's [selected storage map](storage-map.md) allocates 29 native
BSRAM blocks, including host, shared-memory payload slots, record/row staging
and scanout. Its separate register/ROM choices and port schedules do not
establish whole-device fit or achieved clock frequency.

## DSP configurations

A DSP block contains two macros. Each macro contains two 18×18 multipliers,
pre-adders, and a three-input 54-bit ALU. The documented multiplier alternatives
share those resources: one macro provides two 18×18, four 9×9, or one 18×36;
combining two macros provides one 36×36. These are alternative configurations,
not independently available multiplier pools.

The first modeled configuration is deliberately specific:

| Property | Selected `Mult18x18` behavior |
| --- | --- |
| Operands | Two signed 18-bit parallel inputs, range −131,072 through 131,071 |
| Result | Full signed 36-bit product |
| Registers | Operand registers, product register, and output register enabled |
| Latency | Result visible on the third enabled rising edge, counting operand capture |
| Clock enable | Shared CE; a disabled edge holds every stage |
| Reset | Active-high synchronous reset, priority over CE; clears all registers |
| Initial state | Output is unknown until reset or sufficient enabled edges |

This matches the registered signed example in SUG949 §10.1.1 and the register
choices of `MULT18X18` in UG287 §4.2.2. Arbitrary sign-control timing, shift
cascades, bypassed registers, other multiplier sizes, and asynchronous reset are
outside this first model. The implementation rejects captured host operands that
exceed the selected input width.

The model is in [mult18x18.hpp](../../reference/include/nano_gpu/hw/mult18x18.hpp).
A future wider operation must explicitly compose supported primitives and account
for their registers, shared resources, and schedule. A C++ wide multiply is not
an implicit FPGA resource.

## BSRAM configurations

The device supports SP, SDP, true dual-port, and ROM configurations. In SP/SDP,
the 16-Kbit arrangements include widths 1, 2, 4, 8, 16, and 32; the 18-Kbit
arrangements include widths 9, 18, and 36. True dual-port data widths stop at 18.
The GW2AR's 55-nm BSRAM does not support read-before-write mode.

The first model selects the documented **SDPB 1024×16** configuration:

| Property | Selected `Sdp1kx16` behavior |
| --- | --- |
| Physical allocation | One 18-Kbit BSRAM block |
| Ports | Port A writes; port B reads; common clock |
| Data/word address | 16-bit word, 10-bit word index |
| Configuration | `BIT_WIDTH_0=BIT_WIDTH_1=16`, `READ_MODE=0`, `RESET_MODE="SYNC"` |
| Read | Synchronous BYPASS read; output changes after the enabled rising edge |
| Write | Full word at the enabled rising edge |
| Same-address read/write | Forbidden; the software model reports an error before mutation |
| Reset | Clears the output latch and preserves every stored word |
| Initialization | Explicit zero `INIT_RAM_00` through `INIT_RAM_3F` |

The wrapper ties `RESETA` low, uses `CEA=write_enable` and
`CEB=read_enable || reset`, and drives `RESETB` from its reset input. Block selects
are zero. OCE has no effect in BYPASS mode. Writes during reset are excluded.
The output is treated as unknown until a read or reset establishes it.

UG285's full-word ×16 example packs native addresses as
`ADA={word[9:0],2'b00,2'b11}` and `ADB={word[9:0],4'b0000}`. The low write bits
enable both bytes; they cannot simply be omitted. The word-level wrapper does
not expose every native pin or parameter combination. See
[bsram.hpp](../../reference/include/nano_gpu/hw/bsram.hpp).

## Selected resident tile

One 32×32 RGB565/D24 tile uses three instances of that SDPB configuration:

| Bank | Stored bits per pixel | Allocation |
| --- | ---: | --- |
| Color | RGB565, 16 bits | One 1024×16 SDPB |
| Depth low | Depth bits 0–15 | One 1024×16 SDPB |
| Depth high | Depth bits 16–23; remaining word bits zero | One 1024×16 SDPB |

This allocates three physical BSRAM blocks, including unused capacity. Spare
bits do not create extra ports. The first serial rendering profile reserved one 18×18-equivalent multiplier
shared between edge initialization and RGB texture modulation. The selected
signed primitive represents unsigned byte operands and their complete 8×8
product. Exact division by 255 uses bounded shift/add rounding after that
product. The current profile uses separate pipelined instances as described
below.

## Current textured rendering profile

`N20K-HW-RENDER-0.3` selects **fourteen native BSRAM blocks and 46 native
18×18 multiplier equivalents**:

| Allocation | BSRAM blocks | Multiplier equivalents |
| --- | ---: | ---: |
| Resident color/depth tile | 3 | 0 |
| Texture cache / shared copy row | 1 | 0 |
| Fragment commit metadata, factors and results | 6 | 0 |
| Shared-reciprocal seed ROM | 4 | 40 |
| Triangle edge initialization | 0 | 1 |
| Streaming pitched-texture address | 0 | 2 |
| Parallel RGB shading | 0 | 3 |

The 64-entry fragment ring uses synchronous context-memory ports and ordered
retirement. The reciprocal unit reconstructs both U and V in 26 enabled edges
with initiation interval one. Its native partial products and bounded exact
remainder correction preserve canonical signed Q16 rounding. The four ROM
banks hold fixed initialization data; reset clears output/valid state without
rewriting or clearing the memory words. See the
[arithmetic design and proof](reciprocal-pipeline.md).

The streaming texture unit retains the same 64-line, 16-texel direct-mapped
cache. Two registered native multipliers form `y * pitch + x`; tag lookup,
synchronous cache read and later capture form a seven-edge hit pipeline with
initiation interval one. At most eight requests/reservations and one external
miss are retained. A miss freezes younger address/DSP stages while older
results drain, fills the native cache port, then replays the held lookup.
The system profile uses real modeled SDRAM service; the standalone backing
adapter supplies fixed waits and refill words. See
[texture.hpp](../../reference/include/nano_gpu/hw/texture.hpp).

Three native multipliers shade R, G and B in parallel. Replacement and sprites
use factors of 255 through those same stages, preserving raw RGB565. Explicit
image/framebuffer copies reuse the cache bank while rasterization is idle;
a complete clipped source row is buffered before writes and safe row ordering
preserves overlap. They do not allocate an additional renderer bank.

These are selected resources, not a whole-GPU fit claim. The
[RTL qualification report](rtl-qualification.md) records synthesis and routing
of the reciprocal prototype separately from complete-system qualification.

## Retained serial oracles

The historical `N20K-HW-RENDER-0.2` profile used four physical BSRAM blocks and
one shared multiplier. Its serial texture interface retains the ten-edge
shift/add address generator, 14-edge hit schedule and 34-edge idealized miss
schedule for standalone unit comparisons. Those are not current GPU timings.

The user-logic
[PerspectiveDivider](../../reference/include/nano_gpu/hw/perspective.hpp)
also remains an exact arithmetic oracle, not an assumed Gowin hard-divider
primitive. Its restoring algorithm evaluates
`RNE((attribute * 2^26) / q)` using a 90-bit logical numerator, 38-bit divisor
and remainder, and signed 32-bit Q16 result. One capture edge, 90 bit edges and
one rounding edge total 92 enabled edges per coordinate. Current GPU execution
uses the shared-reciprocal pipeline instead of this U-then-V serial schedule.

## Source revisions

The [complete system model](system-model.md) selects **39 of 46 native
SDPB 1024×16 banks**: fourteen rendering banks, two scanout banks, five host-link
banks, one reply-serializer bank, twelve client-slot banks, two shared-reader
banks and three row-staging banks. The [storage map](storage-map.md) records
ports, meaningful capacity, unused capacity and bounded register fields.
Neither the bank count nor the 46-of-48 multiplier budget proves complete
LUT/FF fit or achieved GPU clock.

Page numbers below are printed document pages, not PDF viewer indices. Revision
and section references matter because the CDN URLs may later serve newer files.

| Primary source | Revision and relevant sections |
| --- | --- |
| [Sipeed Tang Nano 20K board documentation](https://wiki.sipeed.com/hardware/en/tang/tang-nano-20k/nano-20k.html) | Board part and resource table |
| [Sipeed device-selection guidance](https://wiki.sipeed.com/hardware/en/tang/Tang-Nano-Doc/questions.html) | GW2AR-18C, QN88, C8/I7 selection |
| [Gowin GW2AR datasheet, DS226](https://cdn.gowinsemi.com.cn/DS226E.pdf) | 2.7E, 2026-07-31; tables 1-1/1-2, §§2.2.1, 2.5, 2.6 |
| [Gowin DSP guide, UG287](https://cdn.gowinsemi.com.cn/UG287E.pdf) | 1.4E, 2025-11-14; §3 p.4, §4.2.2 pp.19–21 |
| [Gowin BSRAM/SSRAM guide, UG285](https://cdn.gowinsemi.com.cn/UG285E.pdf) | 1.4E, 2025-10-17; §3.3 pp.24–28, chapter 4 pp.35–36 |
| [Gowin HDL coding guide, SUG949](https://cdn.gowinsemi.com.cn/SUG949E.pdf) | 2.2E, 2026-06-17; §8.1.2, §10.1.1 pp.110–113 |

Primitive maximum frequencies in the datasheet are not an achieved GPU clock.
This primitive profile does not choose core, SDRAM, or video clocks. The
[system model](system-model.md) selects configurable clock assumptions and
provides timed SDRAM service and scanout. PLL behavior, clock-domain crossings,
and board I/O timing still require physical qualification. HDMI/TMDS signaling
is outside the system model's scope.
