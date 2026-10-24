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
bits do not create extra ports. The rendering backend also reserves one
18×18-equivalent multiplier shared between edge initialization and RGB texture
modulation. The selected signed primitive can represent every unsigned 8-bit
color operand and the complete 8×8 product. Exact division by 255 after the
product uses bounded shift/add rounding logic. Its LUT/FF use and DSP packing
have not been established by synthesis.

## Texture cache and perspective logic

`N20K-HW-RENDER-0.2` adds one SDPB 1024×16 cache bank to the three resident tile
banks, for **four physical BSRAM blocks and one shared 18×18 multiplier**. The
cache has 64 direct-mapped lines of 16 RGB565 words. Tags, validity, and a single
outstanding request are bounded controller state; cache data travels through
the selected synchronous memory ports.

Texture addressing uses ten clocked shift/add steps for `y × pitch + x`.
The selected backing-memory adapter waits four edges on a miss and then
supplies 16 refill words, one per edge. Those delays are model design choices;
they do not represent a measured or fully simulated SDRAM controller. Binding a
different immutable image invalidates cache tags. Reset preserves cache RAM.
See [texture.hpp](../../reference/include/nano_gpu/hw/texture.hpp).

Perspective reconstruction uses the user-logic
[PerspectiveDivider](../../reference/include/nano_gpu/hw/perspective.hpp), not
an assumed Gowin hard-divider primitive. A fixed serial restoring algorithm
evaluates the canonical `RNE((attribute × 2^26) / q)` operation. It consumes a
90-bit logical numerator with 26 constant low bits, uses a 38-bit divisor and
remainder datapath, and produces a signed 32-bit Q16 result with overflow checks.
The selected schedule is one operand-capture edge, 90 bit-processing edges,
and one rounding/result edge: 92 enabled edges per coordinate. Clock disable
holds the complete state. The tile controller captures each registered divider
or texture result on the following common clock edge; these controller edges
add to total tile clocks without changing the unit's own latency.

The row buffer used by explicit image/framebuffer copies reuses the texture
cache bank while rasterization is idle, so the peak allocation remains four
BSRAM blocks. A full clipped source row is buffered before destination writes;
row direction preserves vertically overlapping copies.

This exact serial implementation closes the rendering feature gap. It is a
slow correctness baseline for evaluating later reciprocal pipelines. Its
LUT/register cost, timing, and attainable initiation rate require synthesis.

## Source revisions

The optional [complete system model](system-model.md) additionally instantiates
two scanout FIFO banks, five host-link banks, and one reply-serializer bank:
**12 selected SDPB 1024×16 instances** including the four rendering banks.
Copies reuse the texture bank. Bounded request/response, record, and row staging
also requires register/logic resources. The bank count is not a complete LUT/FF
budget or proof of whole-GPU fit.

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
