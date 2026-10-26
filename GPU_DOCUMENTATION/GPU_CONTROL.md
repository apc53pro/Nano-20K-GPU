# GPU control: CPU interface, commands and examples

This is the CPU programmer's guide for the **implemented RTL**, protocol
**N20K-SYS-0.1**. Read [GPU_ARCHITECTURE.md](GPU_ARCHITECTURE.md) for the rendering
pipeline and CPU/GPU division of work. These two files are the primary GPU
documentation and must be updated with architecture or control changes in the
same change and pushed with those changes to GitHub.

The byte protocol below is implemented, but remains provisional. It is not a
released RP2350 firmware ABI. The electrical pinout, bidirectional pad wrapper,
board timing constraints and RP2350 PIO/DMA driver have **not** been finalized.
The examples implement generic packet/driver logic; their transport callbacks
must be connected to the eventual board interface or a simulator adapter.

The current native-RAM/arithmetic area reduction preserves this **0.1 byte
ABI**: opcodes, capability/status bytes, 320-byte primitive records, numeric
rules and buffer ownership are unchanged. Coefficients, cache tags and SDRAM
write masks have different internal storage, but drivers encode the same
packets and do not compensate for internal pipeline latency or setup clocks.
The source-matched [area-optimization regression](../docs/benchmarks/rtl-fit-optimized.json)
records passing command/copy/LOAD/abort checks and two verified frames of each
demo scene. CPU completion still follows the fence/presentation rules below.

## 1. What the CPU sends

The CPU sends an ordered stream of packets. It uploads textures and prepared
draw records into GPU SDRAM, submits passes, waits for fences, and presents a
completed back buffer. It does not write pixel, triangle or sprite position
registers to directly manipulate persistent display objects.

Each primitive record includes its own render state. Pass descriptors, tile
tables and primitive lists also reside in GPU SDRAM. A command FIFO acceptance
is not completion: use `FENCE` or the corresponding reply/status token before
reusing active resources. Every address below is an offset into **GPU SDRAM**,
not an RP2350 pointer.

## 2. Logical parallel-bus interface

The RTL host interface is source-synchronous. The **RP2350 owns `host_clk`**;
the GPU core clock is a separate domain connected through native asynchronous
FIFOs. Simulation selects approximately 25.2 MHz host clock and 100.8 MHz core
clock. These are nominal profiles, not achieved board timings or a requirement
that every physical host clock have a fixed phase relative to the core.
The source-matched [serial-probe route](../docs/hardware/rtl-core-pnr.json) is
legal but reports a **48.26 MHz core maximum**, failing the 100.8 MHz target;
the host reports 78.89 MHz and passes its 25.2 MHz target. The capability u32
at offset 24 remains 100,800,000; it describes the selected profile,
not timing closure. Production bus pads and electrical timing remain unqualified.

| Signal | Direction at GPU | Current logical meaning |
| --- | --- | --- |
| `host_clk` | Input | Host transfer clock; active rising edge. |
| `host_wr_valid` | Input | A write byte is offered. |
| `host_wr_data[7:0]` | Input | Offered byte. |
| `host_wr_ready` | Output | Offered write will be accepted at the rising edge. |
| `host_rd_req` | Input | Request a response/register byte. |
| `host_reg_addr[6:0]` | Input | Logical stream or read-only bank address. |
| `host_rd_data[7:0]` | Output | Registered returned byte. |
| `host_rd_valid` | Output | Returned byte is valid after the host edge. |
| `abort_request` | Input to core | Out-of-band soft recovery request; internally synchronized. |
| `reset` | Input to core | Hard reset and initialization. |

The separate read/write data ports and seven address bits describe the RTL
boundary. They do **not** establish an eight-data-wire PCB pinout, a final wire
count, an address-latch scheme, or bus-turnaround timing. A production wrapper
must map these signals to the agreed physical bus. Do not assign RP2350 pins
from this logical port table.

### Write and read timing

Set data/control before the rising edge; changing them on the falling edge is
the testbench convention. Do not assert `host_wr_valid` and `host_rd_req`
together. Writes are legal only to logical register 0.

| Host event | Write behavior | Read behavior |
| --- | --- | --- |
| Before rising edge | Present byte, address 0 and `wr_valid=1`. | Present address and `rd_req=1`, with writes disabled. |
| At rising edge | Byte transfers iff `wr_valid && wr_ready` were asserted before the edge. | Read logic advances; a byte may become valid after the edge. |
| After edge | Keep an unaccepted byte and its address stable while valid remains asserted. Advance only after acceptance. | Sample `rd_data` only when returned `rd_valid=1`; the simulator samples at a later falling edge. |
| No acceptance/result | Continue clocking and retain a stalled offered write. | Continue clocking until valid; a status snapshot crosses clock domains and is not an immediate combinational read. |

Each edge with both read request and a valid response can consume a stream byte.
For a single read, deassert the request after observing its result and before
the next rising edge. Do not use `host_wr_ready` as a read-valid indication.
Do not interpret arbitrary old `host_rd_data` as a new reply. Callback and DMA
implementations need finite timeouts and sideband fault checks while waiting.
Electrical setup/hold values and allowable clock rates remain board-validation
work; the C++ model's configurable digital timing checks are not PCB margins.

### Register/bank selection

| Logical address | Write | Read |
| --- | --- | --- |
| 0 | Append one request byte. | Consume one reply-stream byte when valid. |
| 1 | Invalid. | Refresh/latch a coherent 64-byte status snapshot and return status byte 1 with link flags. |
| 2..63 | Invalid. | Read that byte from the latched status snapshot. |
| 64..127 | Invalid. | Read immutable capability byte `address-64`. |

Read register 1 first, then 2..63 to inspect one coherent snapshot. Status byte
0 is reserved zero; it cannot be read through register 0 because that address
selects the reply FIFO. The packet `GET_STATUS` returns all 64 bytes directly.
Link flags added to register 1 are described in section 5.

Sideband reads remain available when a pass, packet parser or `PRESENT` is
blocked. **Packet** `GET_STATUS` and `GET_CAPS` are ordinary ordered commands in
the current RTL; they cannot overtake a long pass or a pending presentation.
Use sideband access for emergency fault/ready polling.

Current logical capacities are 256 RX bytes, two published command leases,
three 1024-byte packet leases, and 1024 TX bytes. The three packet leases use
two physical 2048×8 native RAMs; this sharing does not increase the advertised
payload limit or queue depth. Honor backpressure rather than
assuming a whole packet always fits. There is no packet CRC or automatic
retransmission in this protocol revision.

## 3. Packets and replies

Every multibyte integer is little-endian. Signed fields use two's complement.
No C compiler structure layout or padding is transmitted.

### Request header: 16 bytes

| Offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 4 | `4E 47 50 55`, ASCII `NGPU`; LE u32 `0x5550474E`. |
| 4 | 1 | Major version **0**. |
| 5 | 1 | Minor version **1**. |
| 6 | 2 | Opcode, u16. |
| 8 | 4 | Payload byte length, excluding this header; maximum 1024. |
| 12 | 4 | Request sequence, u32, nonzero and strictly increasing within the epoch. |

The parser publishes only a complete, valid packet. Partial payloads do not
execute. Never insert padding between packets unless that padding belongs to
the declared payload. Never search for a new `NGPU` magic inside failed upload
data; recover out of band instead.

### Reply header and common payload

Replies use the same 16-byte header, with opcode `request_opcode | 0x8000`,
the request sequence, and payload length. Every successful reply starts with:

| Payload offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 4 | Execution epoch. |
| 4 | 4 | Result status, currently always zero for emitted successful replies. |
| 8 | 4 | Token: caller token for `FENCE`, `PRESENT`, `READ_MEMORY`; zero for queries. |
| 12 | Variable | Command-specific extra data. |

Validate magic, version, response opcode, exact length, sequence, epoch, status
and token. The current RTL stops on a fault and does **not** emit a normal
error/NACK reply. A reply wait must therefore time out or detect a sideband
fault and enter recovery. Do not wait forever for a successful fence after a
failed pass. Only one software consumer may drain the reply stream.

### Command table

Offsets in the payload column are relative to the first byte after the header.

| Opcode | Name | Exact request payload | Successful response |
| --- | --- | --- | --- |
| `0x0001` | `GET_CAPS` | Empty. | Common 12 bytes + 64 capability bytes; **92 bytes total**. |
| `0x0002` | `GET_STATUS` | Empty. | Common 12 bytes + 64 status bytes; **92 bytes total**. |
| `0x0003` | `WRITE_HEAP` | u32 destination at 0, u32 data length at 4, then exactly that many bytes at 8. Data length **1..1016**. | No reply; completion through a later fence/status sequence. |
| `0x0004` | `SUBMIT_PASS` | u32 pass-descriptor address at 0; **4 bytes**. | No reply; later fence waits for rendering and stores. |
| `0x0005` | `COPY_RECT` | **56 bytes**, layout in section 8. | No reply; later fence waits for the copy. |
| `0x0006` | `FENCE` | u32 caller token at 0; **4 bytes**. | Common payload only; **28 bytes total**. |
| `0x0007` | `PRESENT` | u32 physical color target at 0, u32 presentation token at 4; **8 bytes**. | Common payload only, after the swap at vertical blank; **28 bytes total**. |
| `0x0008` | `READ_MEMORY` | u32 byte address at 0, u32 length at 4, u32 result token at 8; **12 bytes**. Length **1..512**. | Common payload + u32 requested address + u32 returned length + data; **36+length bytes total**. |

`WRITE_HEAP` destinations may be unaligned, and byte masks preserve adjacent
bytes. Its entire range must be in the heap. `READ_MEMORY` may be unaligned
and may inspect any valid SDRAM range, including attachments; it is diagnostic
readback, not the display data path. Neither command permits address wrap.
There is no normal command opcode for soft abort or hard reset.

`FENCE` orders all earlier commands and their required SDRAM stores. A fence
before `PRESENT` establishes that drawing is finished. A fence after `PRESENT`
also waits for the preceding swap. A reply's bytes becoming available and the
host actually consuming them are different events; drain replies to avoid
blocking later work through a full TX FIFO.

## 4. Memory, passes and buffer ownership

The GPU has 8 MiB of SDRAM. Ranges are half-open:

| Region | Start | End, excluded | Size |
| --- | --- | --- | ---: |
| Physical color 0, RGB565 | `0x000000` | `0x096000` | 614,400 bytes |
| Physical color 1, RGB565 | `0x096000` | `0x12C000` | 614,400 bytes |
| Shared D24 attachment, stored as u32 | `0x12C000` | `0x258000` | 1,228,800 bytes |
| Heap: textures, records, lists, tables, descriptors | `0x258000` | `0x800000` | 5,931,008 bytes |

Color pitch is 1280 bytes; pixel `(x,y)` is at `color_base + 1280*y + 2*x`.
Depth pitch is 2560 bytes, with low 24 bits meaningful and high eight bits zero.
Depth zero is near and `0xFFFFFF` is far; LESS rejects a sample equal to the
clear value, while LEQUAL accepts equality.
`WRITE_HEAP` cannot write attachments. Heap allocation, nonoverlap and resource
lifetime belong to the CPU driver. Check sums with at least 64-bit arithmetic.

After boot initialization, color 0 is front, color 1 is back, both are black,
and depth is `0xFFFFFF`. Render/copy only to the writable non-front buffer;
LOAD and partial copies additionally require valid preserved contents.
`PRESENT` seals its target when it executes, then swaps at vertical blank.
The old front becomes writable after the reply. Never mutate the displayed or
sealed buffer. Late rendering repeats the current front.

Depth is shared and never swapped. After a swap, the new back contains the old
front's color, while depth contains the latest pass's depth. Clear depth for a
new 3D frame unless the application deliberately maintains matching contents.
`LOAD` preserves the named physical buffer, not automatically the latest image.
The CPU may copy front to back first when preservation of that image is needed.

### Pass descriptor: 32 bytes, address aligned to four bytes

| Offset | Type | Value |
| --- | --- | --- |
| 0 | u32 | Magic `0x3153504E`, bytes `4E 50 53 31` (`NPS1`). |
| 4 | u32 | Physical target color ID, 0 or 1. |
| 8 | u32 | Bit 0 color CLEAR, bit 1 depth CLEAR; a clear bit of 0 means LOAD. Other bits zero. |
| 12 | u32 | Clear RGB565 in low 16 bits; upper bits zero. |
| 16 | u32 | Clear D24 in low 24 bits; upper bits zero. |
| 20 | u32 | Four-byte-aligned tile-table heap address. |
| 24 | u32 | Tile count, exactly **300**. |
| 28 | u32 | Reserved zero. |

The table is 300 entries of `{u32 list_address, u32 reference_count}`: **2400
bytes**, in order `tile_index = (y/32)*20 + x/32`. Each referenced list contains
ordered u32 primitive-record addresses. Empty lists have count zero **and**
address zero. Nonempty lists and records are four-byte aligned heap ranges.
Total references per pass must not exceed `1,048,576`; the supplied CPU compiler
additionally limits its input to 16,384 primitive records.

Every tile is visited for LOAD/CLEAR and store, including empty tiles. Each
primitive must appear once in every tile whose clipped bounds overlap it; use
the original logical draw order within each list. It is safe to include a tile
that ultimately has no covered pixels. Do not omit a tile that can contain a
sample. Uploading a record does not bin it automatically.

All descriptor/table/list/record/texture allocations referenced by an active
pass remain immutable until that pass finishes. Ordered updates after a fence
can reuse them. Textures must live in the heap; sampling a framebuffer or the
active target as a texture is unsupported. The RTL invalidates texture cache
state at pass submission, so ordered heap updates are visible to the next pass.

## 5. Status, capabilities and faults

All offsets in this section refer to the 64-byte bank, **not** to packet byte
zero. Packet snapshots begin at total reply byte 28.

### Status bank

| Byte offset | Type | Current RTL value |
| --- | --- | --- |
| 0 | u8 | Reserved zero. |
| 1 | u8 | Core flags: bit 0 ready, bit 1 busy, bit 2 sealed/presentation pending, bit 3 core fault. |
| 2 | u32 | Execution epoch. |
| 6 | u32 | Last completed command sequence. |
| 10 | u32 | Last successful fence token. |
| 14 | u8 | Current physical front color ID. |
| 15 | u8 | Bit 0 color 0 valid, bit 1 color 1 valid, bit 2 depth valid. |
| 16 | u64 | Core cycle counter; restarts on hard reset. |
| 24 | u64 | Scanout underflows; this RTL supplies a 32-bit count and zero upper half. |
| 32 | u32 | Core fault code, currently low eight bits used. |
| 36 | u32 | Last successful presentation token. |
| 40..55 | Bytes | Reserved zero in RTL. |
| 56 | u8 | Transport/parser fault code. |
| 57..63 | Bytes | Reserved zero. |

Sideband register 1 adds bit 7 transport fault, bit 6 RX full, bit 5 published
command queue full, and bit 4 reply available. Queue-full and parser-fault
contributions come from the transferred core snapshot; a local host fault also
sets bit 7 directly. RX-full/reply-available bits are evaluated in the host domain.
Check both core fault bit 3 and transport fault bit 7, plus the diagnostic
fault bytes. The packet `GET_STATUS` returns the raw core snapshot and does
not add these upper four host-link flag bits. Reserved RTL bytes must not be
interpreted as the extra diagnostic counters provided by the older C++ model.

`busy` includes command processing; a packet `GET_STATUS` snapshot can itself
show busy. `ready` means initialization/cross-domain link recovery permits the
core to run; it does **not** alone mean fault-free, idle, or abort-complete.

An ordered `GET_STATUS` reply remains one coherent snapshot during slow reply
draining. The implementation captures independently changing counters and
flags, and holds execution state stable until serialization finishes.
`GET_CAPS` serializes immutable constants directly. This internal storage
sharing does not make either reply a series of unrelated live register reads.
Sideband reads use the acknowledged core snapshot bank, which stays unchanged
until a new refresh request; refresh blocks reads until its acknowledgement.
There is no second host-domain copy of all 64 bytes.

### Capability bank

| Byte offset | Type | Current value / meaning |
| --- | --- | --- |
| 0 | u8 | Major 0. |
| 1 | u8 | Minor 1. |
| 2 | u8 | Bus data width 8. |
| 3 | u8 | Tile side 32. |
| 4 | u32 | SDRAM bytes: `0x800000`. |
| 8 | u32 | Heap base: `0x258000`. |
| 12 | u32 | Maximum request payload: 1024. |
| 16 | u32 | Primitive record bytes: 320. |
| 20 | u32 | Maximum pass references: 1,048,576. |
| 24 | u32 | Nominal core Hz: 100,800,000; not measured Fmax. |
| 28 | u32 | Selected host-divider metadata: 4; the physical host still supplies its clock. |
| 32 | u32 | Selected pixel divider: 4. |
| 36 | u32 | RX FIFO bytes: 256. |
| 40 | u32 | Published command capacity: 2. |
| 44 | u32 | TX FIFO bytes: 1024. |
| 48 | u32 | Display FIFO pixels: 2048. |
| 52 | u32 | Width: 640. |
| 56 | u32 | Height: 480. |
| 60 | u32 | Current feature mask `0x000000FF`; do not infer future features from reserved bits. |

### Fault codes and recovery

Core fault codes are: 1 transport, 2 payload/opcode, 3 address/range, 4 buffer
ownership or COPY arguments, 5 invalid LOAD contents, 6 descriptor/record/
texture/numeric failure, 7 reference limit/list failure, 8 SDRAM client error,
9 internal state, and `0xFF` exhausted epoch counter. The code is a category,
not a full failing-address report. Several structural failures share code 6.

Current host RTL emits: 1 simultaneous read/write, 2 write to nonzero register,
5 changed stalled write byte/address, 7 magic, 8 version, 9 opcode, 10 packet
length, 11 non-increasing/zero sequence. Do not assume the C++ model's additional
digital setup/hold fault codes are measured by this RTL.

Recovery procedure:

1. Stop/quiesce host packet DMA and deassert write/read requests. Discard local
   expectations for unfinished replies; stop submitting new work.
2. Request out-of-band soft abort. Keep **both clocks running** while accepted
   memory work drains and the cross-domain RX/TX/parser flush completes.
3. Through sideband reads, wait for a **changed epoch**, ready asserted and
   fault indications cleared. Ready alone is insufficient. The physical abort
   pulse/ack and reset timing must be finalized in the board wrapper.
4. Discard any old local reply bytes, reset sequence allocation to 1, and
   invalidate the CPU's assumptions about interrupted resources. An interrupted
   heap write leaves its touched range unspecified; reupload it before use.
5. If back/depth validity was lost, submit a full CLEAR/CLEAR pass before LOAD
   or presentation. Preserve the front until valid new work completes.

Soft recovery increments the 32-bit epoch. It cannot recover past
`0xFFFFFFFF`; hard reset is then required. **Hard reset sets epoch to 1 again**,
reinitializes attachments, and invalidates heap assumptions. Therefore epoch
is not a globally unique power-cycle identifier: a reconnecting driver must
establish a fresh transport session and discard stale bytes/completions.

## 6. Primitive record format: 320 bytes

Zero the entire record before encoding it. Primitive kinds are triangle 0,
rectangle 1, line 2, sprite 3. Version is 1. All unused/reserved bytes and bits
must be zero.

| Offset | Type / size | Meaning |
| --- | --- | --- |
| 0 | u32 | Kind bits 0..7; version bits 8..15; color write bit 16, depth test bit 17, depth write bit 18, color-key enable bit 19. Bits 20..31 zero. |
| 4 | u32 | Shade mode bits 0..7; depth function bits 8..15; U REPEAT bit 16; V REPEAT bit 17; texture descriptor present bit 18. Bits 19..31 zero. |
| 8 | u32 | Constant RGB888: R in bits 0..7, G in 8..15, B in 16..23; high byte zero. |
| 12 | u32 | Raw RGB565 color key in low 16 bits; zero if keying disabled; high half zero. |
| 16 | i32 × 4 | Scissor `{x0,y0,x1,y1}`, half-open. |
| 32 | i32 × 4 | Primitive bounds `{x0,y0,x1,y1}`; all zero for a line. |
| 48 | u32 × 4 | Texture `{base,width,height,pitch_bytes}`; all zero if descriptor absent. |
| 64 | u32 | Constant D24 for 2D; zero for a triangle. |
| 68,72 | i32, i32 | Sprite source X/Y or line endpoint A X/Y; zero for triangle/rectangle. |
| 76 | u32 | Reserved zero. |
| 80 | i32 × 6 | Triangle Q4 vertex `{X0,Y0,X1,Y1,X2,Y2}`. For a line only offsets 80/84 hold endpoint B X/Y; remaining bytes zero. |
| 104 | 3 × 16 bytes | Triangle edges `{i32 A,i32 B,i32 C,u32 inclusive}`; inclusive is 0 or 1. |
| 152 | 7 × 24 bytes | Triangle planes in order **z,r,g,b,q,U,V**; each `{i64 seed,i64 step_x,i64 step_y}`. |

Shade modes: 0 FLAT, 1 GOURAUD, 2 TEXTURE_REPLACE, 3 TEXTURE_FLAT_MODULATE,
4 TEXTURE_GOURAUD_MODULATE. Depth functions: 0 LESS, 1 LEQUAL, 2 ALWAYS.
Each address axis uses CLAMP when its REPEAT bit is zero.

Scissors, rectangle/sprite bounds, sprite source origins and line endpoints
use integer pixels. Bounds are half-open; a 1×1 rectangle draws one pixel.
Line endpoints must be CPU-clipped inside the viewport before encoding. Lines
include both endpoints and use the global line sequence in the
[numeric contract](../docs/spec/rasterization.md#2d-coverage); never restart the
line from separately clipped tile endpoints. Bin a line using the inclusive
endpoint bounding box converted to half-open bounds, intersected with scissor
and viewport, even though its record's bounds field is zero.

Rectangles and lines support flat, untextured, unkeyed color. Sprites support
raw 1:1 texture replacement with optional key and constant depth; mode 0 is also
accepted as a raw sprite alias, but new drivers should use mode 2. Sprites have
no scaling, flipping, rotation or Gouraud/modulated shading. Copies are a
different command, not shaded primitives. Color/depth writes and depth testing
are independently enabled. Key rejection suppresses **both** attachment writes.

For triangles, only textured modes may enable color keying. The CPU setup/encoder rejects
untextured keying; the current raw RTL validator has a known gap and can ignore
that invalid flag. Drivers must reject it instead of relying on accidental
acceptance. Supported combinations are not an exhaustive proof of all possible
state combinations.

Texture dimensions are powers of two in 1..1024; pitch is at least `2*width`
and divisible by four. Base is four-byte aligned and the full `pitch*height`
extent, including last-row padding, lies in the heap. Format is RGB565, stored
little-endian. Nearest sampling uses floor before per-axis clamp/repeat. Raw
color key comparison occurs before RGB expansion or modulation. Sprite source
rectangles must fit the texture even when destination clipping removes pixels.
A textured triangle needs its descriptor when color writes or keying require
sampling. A depth-only triangle with both disabled may omit texture sampling
and the descriptor even if a textured shade mode is selected. Sprites always
require a compatible descriptor.

### Preparing triangle records

The CPU transforms, clips, projects, snaps vertices to Q4, normalizes winding,
computes edges/bounds and prepares the attribute planes. It also performs any
lighting used to produce constant or vertex RGB values. No vertex/index buffer
or projection matrix command exists in this revision.

For sample anchor `(x0,y0)` from the triangle's clipped bounds, each plane is
`seed + (x-x0)*step_x + (y-y0)*step_y`. Depth is D24-code units with 16 fractional
bits; RGB channel planes have 16 fractional bits; q has scale `2^37`; U=u*q and
V=v*q have scale `2^27`. All stored values are signed 64-bit two's complement.
Coverage uses unbiased edges at pixel centers and top-left inclusion. The
maximum W ratio is 64 in the current setup profile; normalized vertex q is
in `[1/128,1/2]`, and covered reconstructed q must be `[1/256,1]`.

Use the complete equations, overflow checks and RNE rules in the
[numeric contract](../docs/spec/rasterization.md). Do not invent coefficient
scales from these short descriptions. The desktop
[`setup_triangle()` implementation](../reference/src/setup.cpp) is an executable
reference, not a ready-to-flash RP2350 driver: it uses host facilities including
`__int128`, which typical 32-bit RP2350 GCC targets do not provide. An RP2350
port needs suitable bounded/wide arithmetic or a validated equivalent setup.
The fixed prepared triangle in section 9 avoids that portability requirement
for an initial interface test.

## 7. Frame sequence

For a new session, first establish initialization and a fault-free epoch through
sideband status, then query capabilities. A simple serialized frame is:

```text
WRITE_HEAP(texture pixels, only when new or changed)
WRITE_HEAP(immutable primitive records)
WRITE_HEAP(ordered reference lists, 300-entry table, pass descriptor)
SUBMIT_PASS(descriptor address)
FENCE(render token) -> wait for matching successful reply
PRESENT(back buffer, presentation token) -> wait for matching reply
```

Reuse resident texture allocations after their prior users finish. The current
RTL demo's standalone pass compiler still reuploads its assets each frame;
that convenience behavior is not a protocol requirement. More advanced drivers
can queue work safely, but must preserve sequence/epoch identity and physical
buffer ownership. Do not modify the active pass's records or lists in place.

## 8. COPY_RECT payload: 56 bytes

All fields are four bytes and little-endian. Coordinates and requested
width/height are signed i32; widths/heights must be nonnegative.

| Offset | Field |
| --- | --- |
| 0 | u32 source kind: 0 physical framebuffer, 1 heap image. |
| 4 | u32 source framebuffer ID 0/1; must be 0 for heap images. |
| 8 | u32 target framebuffer ID 0/1. |
| 12,16 | i32 source X/Y. |
| 20,24 | i32 destination X/Y. |
| 28,32 | i32 requested width/height. |
| 36,40,44,48 | u32 heap image base/width/height/pitch; zero for framebuffer source. |
| 52 | Reserved zero. |

Heap image dimensions may be non-power-of-two, in 1..1024, with aligned base/
pitch, pitch at least twice the width, and the full extent inside the heap.
The target must be writable back; front is allowed as a read-only source.
Source/destination clipping preserves their pixel correspondence. Same-buffer
overlap has snapshot/memmove semantics. Zero-area copies are no-ops. Copies
do not apply keying, shading, filtering or depth operations. A partial copy
does not initialize an invalid target; a full destination-covering copy can.
The current executor validates the prefix one full raw word at a time, then
refetches 14 COPY parameter words from its immutable packet lease. X and Y
reuse one signed 33-bit interval/clipping datapath. For a nonempty copy, that
refetch/clipping phase takes about 128 core clocks before address walking and
pixel transfers. Completion is determined by the ordered fence/reply rules,
not a fixed CPU delay. Native coefficient capture adds one setup clock per
nonempty primitive/tile invocation; the sampler's private exact integer-texel
pipeline has 18 enabled-edge latency and initiation interval one. Neither
change modifies prepared records, numeric rules or completion semantics.

## 9. Generic C99 driver examples

The following single C99 block is self-contained packet/heap logic. It requires
only standard C headers and the three callbacks in `GpuTransport`. It contains
no GPIO numbers, PIO program, host timing delays, or SDK-specific calls.

Callback contract:

- `write_byte`: transfer exactly one byte to register 0, respecting valid/ready;
  return true only after acceptance.
- `read_byte`: consume exactly one valid register-0 reply byte, sampling after
  the host edge; return true only after that byte is received.
- `read_register`: issue one read of a nonzero logical register, wait for its
  registered valid result, and deassert the request before another transfer.
- Every callback must have a finite timeout, continue clocking as required,
  and return false on transport failure. The HAL must keep clocks running for
  cross-domain progress; it must implement abort/reset separately.

The helper is intentionally serialized and allows one expected reply at a time.
After a partial transfer or invalid reply it marks the link failed; perform the
recovery procedure before reinitializing it. `gpu_init()` only initializes CPU
state and must be used with a fresh/quiesced transport session, not to restart
sequence numbers against a GPU that is still processing old commands.

Its scratch layout reserves 3952 heap bytes for one record/pass. A real driver
should replace this fixed allocation with its allocator. Call `gpu_submit_one`
only after a prior fence has released this scratch range. It clears both
attachments and bins one primitive; this is a small complete example, not an
efficient many-object renderer.

<!-- gpu-control-c99:start -->
```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    GPU_GET_CAPS = 1, GPU_GET_STATUS = 2, GPU_WRITE_HEAP = 3,
    GPU_SUBMIT_PASS = 4, GPU_COPY_RECT = 5, GPU_FENCE = 6,
    GPU_PRESENT = 7, GPU_READ_MEMORY = 8,
    GPU_WIDTH = 640, GPU_HEIGHT = 480, GPU_TILES = 300,
    GPU_RECORD_BYTES = 320, GPU_MAX_PAYLOAD = 1024,
    GPU_MAX_REPLY_PAYLOAD = 532
};
#define GPU_MEMORY_BYTES UINT32_C(0x800000)
#define GPU_HEAP_BASE UINT32_C(0x258000)
#define GPU_TABLE_ADDR GPU_HEAP_BASE
#define GPU_RECORD_ADDR (GPU_TABLE_ADDR + UINT32_C(2400))
#define GPU_REFS_ADDR (GPU_RECORD_ADDR + UINT32_C(320))
#define GPU_PASS_ADDR (GPU_REFS_ADDR + UINT32_C(1200))
#define GPU_TEXTURE_ADDR (GPU_PASS_ADDR + UINT32_C(32))

typedef struct {
    void *user;
    bool (*write_byte)(void *, uint8_t);
    bool (*read_byte)(void *, uint8_t *);
    bool (*read_register)(void *, uint8_t, uint8_t *);
} GpuTransport;
typedef struct {
    GpuTransport transport;
    uint32_t next_sequence, epoch;
    bool failed;
} Gpu;
typedef struct {
    uint32_t payload_bytes;
    uint8_t payload[GPU_MAX_REPLY_PAYLOAD];
} GpuReply;
typedef struct { int32_t x0, y0, x1, y1; } GpuBounds;

void gpu_put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
void gpu_put32(uint8_t *p, uint32_t value) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
void gpu_put64(uint8_t *p, int64_t value) {
    uint64_t bits = (uint64_t)value;
    unsigned i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)(bits >> (8 * i));
}
uint32_t gpu_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void gpu_init(Gpu *g, GpuTransport transport) {
    memset(g, 0, sizeof(*g)); g->transport = transport; g->next_sequence = 1;
}
bool gpu_fail(Gpu *g) { g->failed = true; return false; }
bool gpu_heap_range(uint32_t address, uint32_t length) {
    return address >= GPU_HEAP_BASE &&
           (uint64_t)address + length <= GPU_MEMORY_BYTES;
}
bool gpu_read_bytes(Gpu *g, uint8_t *bytes, uint32_t length) {
    uint32_t i;
    if (g->failed || !g->transport.read_byte) return false;
    for (i = 0; i < length; ++i)
        if (!g->transport.read_byte(g->transport.user, bytes + i)) return gpu_fail(g);
    return true;
}
bool gpu_snapshot(Gpu *g, uint8_t status[64]) {
    unsigned i;
    if (g->failed || !g->transport.read_register) return false;
    status[0] = 0;
    for (i = 1; i < 64; ++i)
        if (!g->transport.read_register(g->transport.user, (uint8_t)i, status + i))
            return gpu_fail(g);
    return true;
}
bool gpu_send(Gpu *g, uint16_t opcode, const uint8_t *payload,
              uint32_t length, uint32_t *sequence) {
    uint8_t header[16];
    uint32_t i, seq;
    if (g->failed || !g->transport.write_byte || !sequence ||
        opcode < 1 || opcode > 8 || length > GPU_MAX_PAYLOAD ||
        (length && !payload) || g->next_sequence == 0 ||
        g->next_sequence == UINT32_MAX) return false;
    seq = g->next_sequence++; /* Reserve the final value for planned recovery. */
    gpu_put32(header, UINT32_C(0x5550474e));
    header[4] = 0; header[5] = 1;
    gpu_put16(header + 6, opcode); gpu_put32(header + 8, length);
    gpu_put32(header + 12, seq);
    for (i = 0; i < 16; ++i)
        if (!g->transport.write_byte(g->transport.user, header[i])) return gpu_fail(g);
    for (i = 0; i < length; ++i)
        if (!g->transport.write_byte(g->transport.user, payload[i])) return gpu_fail(g);
    *sequence = seq; return true;
}
bool gpu_reply(Gpu *g, uint16_t opcode, uint32_t sequence,
               uint32_t token, uint32_t extra_bytes, GpuReply *reply) {
    uint8_t header[16];
    uint32_t length, epoch;
    uint16_t returned_opcode;
    if (!reply || extra_bytes > GPU_MAX_REPLY_PAYLOAD - 12 ||
        !gpu_read_bytes(g, header, 16)) return false;
    length = gpu_get32(header + 8);
    returned_opcode = (uint16_t)((uint16_t)header[6] | ((uint16_t)header[7] << 8));
    if (gpu_get32(header) != UINT32_C(0x5550474e) || header[4] != 0 || header[5] != 1 ||
        returned_opcode != (uint16_t)(opcode | UINT16_C(0x8000)) ||
        gpu_get32(header + 12) != sequence || length != 12 + extra_bytes)
        return gpu_fail(g);
    if (!gpu_read_bytes(g, reply->payload, length)) return false;
    epoch = gpu_get32(reply->payload);
    if (!epoch || (g->epoch && epoch != g->epoch) ||
        gpu_get32(reply->payload + 4) != 0 || gpu_get32(reply->payload + 8) != token)
        return gpu_fail(g);
    g->epoch = epoch; reply->payload_bytes = length; return true;
}
bool gpu_query(Gpu *g, uint16_t opcode, uint8_t result[64]) {
    uint32_t sequence;
    GpuReply reply;
    if ((opcode != GPU_GET_CAPS && opcode != GPU_GET_STATUS) ||
        !gpu_send(g, opcode, NULL, 0, &sequence) ||
        !gpu_reply(g, opcode, sequence, 0, 64, &reply)) return false;
    memcpy(result, reply.payload + 12, 64); return true;
}
bool gpu_start_session(Gpu *g, GpuTransport transport, unsigned max_polls,
                       uint8_t caps[64], uint8_t status[64]) {
    unsigned poll;
    gpu_init(g, transport);
    for (poll = 0; poll < max_polls; ++poll) {
        if (!gpu_snapshot(g, status)) return false;
        if ((status[1] & UINT8_C(0x88)) || status[56]) return gpu_fail(g);
        if (status[1] & 1) {
            g->epoch = gpu_get32(status + 2);
            if (!g->epoch || !gpu_query(g, GPU_GET_CAPS, caps)) return false;
            return caps[0] == 0 && caps[1] == 1 && caps[2] == 8 && caps[3] == 32 &&
                   gpu_get32(caps + 4) == GPU_MEMORY_BYTES &&
                   gpu_get32(caps + 8) == GPU_HEAP_BASE &&
                   gpu_get32(caps + 12) == GPU_MAX_PAYLOAD &&
                   gpu_get32(caps + 16) == GPU_RECORD_BYTES;
        }
    }
    return false;
}
bool gpu_upload(Gpu *g, uint32_t address, const uint8_t *data, uint32_t length) {
    uint8_t payload[GPU_MAX_PAYLOAD];
    uint32_t cursor = 0, sequence;
    if (!gpu_heap_range(address, length) || (length && !data)) return false;
    while (cursor < length) {
        uint32_t chunk = length - cursor;
        if (chunk > 1016) chunk = 1016;
        gpu_put32(payload, address + cursor); gpu_put32(payload + 4, chunk);
        memcpy(payload + 8, data + cursor, chunk);
        if (!gpu_send(g, GPU_WRITE_HEAP, payload, chunk + 8, &sequence)) return false;
        cursor += chunk;
    }
    return true; /* A zero-length CPU upload emits no packet. */
}
bool gpu_fence(Gpu *g, uint32_t token) {
    uint8_t payload[4]; uint32_t sequence; GpuReply reply;
    gpu_put32(payload, token);
    return gpu_send(g, GPU_FENCE, payload, 4, &sequence) &&
           gpu_reply(g, GPU_FENCE, sequence, token, 0, &reply);
}
bool gpu_present(Gpu *g, uint32_t target, uint32_t token) {
    uint8_t payload[8]; uint32_t sequence; GpuReply reply;
    if (target > 1) return false;
    gpu_put32(payload, target); gpu_put32(payload + 4, token);
    return gpu_send(g, GPU_PRESENT, payload, 8, &sequence) &&
           gpu_reply(g, GPU_PRESENT, sequence, token, 0, &reply);
}
bool gpu_read_memory(Gpu *g, uint32_t address, uint32_t length,
                     uint32_t token, uint8_t *data) {
    uint8_t payload[12]; uint32_t sequence; GpuReply reply;
    if (!data || !length || length > 512 || (uint64_t)address + length > GPU_MEMORY_BYTES)
        return false;
    gpu_put32(payload, address); gpu_put32(payload + 4, length); gpu_put32(payload + 8, token);
    if (!gpu_send(g, GPU_READ_MEMORY, payload, 12, &sequence) ||
        !gpu_reply(g, GPU_READ_MEMORY, sequence, token, 8 + length, &reply)) return false;
    if (gpu_get32(reply.payload + 12) != address || gpu_get32(reply.payload + 16) != length)
        return gpu_fail(g);
    memcpy(data, reply.payload + 20, length); return true;
}
void gpu_put_bounds(uint8_t *p, GpuBounds bounds) {
    gpu_put32(p, (uint32_t)bounds.x0); gpu_put32(p + 4, (uint32_t)bounds.y0);
    gpu_put32(p + 8, (uint32_t)bounds.x1); gpu_put32(p + 12, (uint32_t)bounds.y1);
}
void gpu_record_base(uint8_t record[320], uint32_t kind, uint32_t flags) {
    GpuBounds viewport = {0, 0, 640, 480};
    memset(record, 0, 320);
    gpu_put32(record, kind | UINT32_C(0x100) | (flags << 16));
    gpu_put_bounds(record + 16, viewport);
}
void gpu_make_rect(uint8_t record[320], GpuBounds bounds, uint32_t rgb888) {
    gpu_record_base(record, 1, 1); /* color write; no depth test/write or key */
    gpu_put32(record + 8, rgb888 & UINT32_C(0xffffff));
    gpu_put_bounds(record + 32, bounds);
}
void gpu_make_example_triangle(uint8_t record[320]) {
    const int32_t vertex[6] = {0, 0, 256, 0, 0, 256}; /* Q4: (0,0),(16,0),(0,16) */
    const int32_t edges[3][4] = {{0, 256, 0, 1}, {-256, -256, 65536, 0}, {256, 0, 0, 1}};
    GpuBounds bounds = {0, 0, 16, 16};
    unsigned i, j;
    gpu_record_base(record, 0, 7); /* color+depth test+depth write, LESS, FLAT */
    gpu_put32(record + 8, UINT32_C(0x2040ff));
    gpu_put_bounds(record + 32, bounds);
    for (i = 0; i < 6; ++i) gpu_put32(record + 80 + 4 * i, (uint32_t)vertex[i]);
    for (i = 0; i < 3; ++i) for (j = 0; j < 4; ++j)
        gpu_put32(record + 104 + 16 * i + 4 * j, (uint32_t)edges[i][j]);
    gpu_put64(record + 152, INT64_C(0x4000000000)); /* Constant D24 0x400000, Q16. */
}
bool gpu_make_sprite(uint8_t record[320], GpuBounds bounds, int32_t source_x,
                     int32_t source_y, uint32_t base, uint32_t width,
                     uint32_t height, uint32_t pitch, bool key_on, uint16_t key) {
    int64_t draw_width = (int64_t)bounds.x1 - bounds.x0;
    int64_t draw_height = (int64_t)bounds.y1 - bounds.y0;
    if (!width || !height || width > 1024 || height > 1024 ||
        (width & (width - 1)) || (height & (height - 1)) ||
        (base & 3) || (pitch & 3) || pitch < width * 2 ||
        (uint64_t)base + (uint64_t)pitch * height > GPU_MEMORY_BYTES || base < GPU_HEAP_BASE ||
        source_x < 0 || source_y < 0 || draw_width < 0 || draw_height < 0 ||
        (draw_width && draw_height &&
         ((int64_t)source_x + draw_width > width || (int64_t)source_y + draw_height > height)))
        return false;
    gpu_record_base(record, 3, 1u | (key_on ? 8u : 0u));
    gpu_put32(record + 4, 2u | (1u << 18)); /* REPLACE + descriptor present */
    gpu_put32(record + 12, key_on ? key : 0);
    gpu_put_bounds(record + 32, bounds);
    gpu_put32(record + 48, base); gpu_put32(record + 52, width);
    gpu_put32(record + 56, height); gpu_put32(record + 60, pitch);
    gpu_put32(record + 68, (uint32_t)source_x); gpu_put32(record + 72, (uint32_t)source_y);
    return true;
}
bool gpu_submit_one(Gpu *g, uint32_t target, uint16_t clear565,
                    const uint8_t *record, GpuBounds bounds) {
    uint8_t table[2400] = {0}, refs[1200] = {0}, descriptor[32] = {0}, payload[4];
    int32_t x0 = bounds.x0 < 0 ? 0 : bounds.x0, y0 = bounds.y0 < 0 ? 0 : bounds.y0;
    int32_t x1 = bounds.x1 > 640 ? 640 : bounds.x1, y1 = bounds.y1 > 480 ? 480 : bounds.y1;
    uint32_t sequence;
    if (target > 1) return false;
    if (record) {
        int32_t tx, ty;
        if (!gpu_upload(g, GPU_RECORD_ADDR, record, 320)) return false;
        if (x0 < x1 && y0 < y1) for (ty = y0 / 32; ty <= (y1 - 1) / 32; ++ty)
            for (tx = x0 / 32; tx <= (x1 - 1) / 32; ++tx) {
                uint32_t tile = (uint32_t)ty * 20u + (uint32_t)tx;
                gpu_put32(refs + 4 * tile, GPU_RECORD_ADDR);
                gpu_put32(table + 8 * tile, GPU_REFS_ADDR + 4 * tile);
                gpu_put32(table + 8 * tile + 4, 1);
            }
        if (!gpu_upload(g, GPU_REFS_ADDR, refs, sizeof(refs))) return false;
    }
    if (!gpu_upload(g, GPU_TABLE_ADDR, table, sizeof(table))) return false;
    gpu_put32(descriptor, UINT32_C(0x3153504e)); gpu_put32(descriptor + 4, target);
    gpu_put32(descriptor + 8, 3); /* color CLEAR + depth CLEAR */
    gpu_put32(descriptor + 12, clear565); gpu_put32(descriptor + 16, UINT32_C(0xffffff));
    gpu_put32(descriptor + 20, GPU_TABLE_ADDR); gpu_put32(descriptor + 24, GPU_TILES);
    if (!gpu_upload(g, GPU_PASS_ADDR, descriptor, sizeof(descriptor))) return false;
    gpu_put32(payload, GPU_PASS_ADDR);
    return gpu_send(g, GPU_SUBMIT_PASS, payload, 4, &sequence);
}
bool gpu_patch_bounds(Gpu *g, uint32_t record_address, GpuBounds bounds) {
    uint8_t bytes[16];
    if ((record_address & 3) || !gpu_heap_range(record_address, 320)) return false;
    gpu_put_bounds(bytes, bounds);
    return gpu_upload(g, record_address + 32, bytes, sizeof(bytes));
}
```
<!-- gpu-control-c99:end -->

### Using the helpers

The snippet below uses the C99 helpers above. `transport` is the application's
initialized `GpuTransport`, and its physical link must already be fresh and
quiesced. A real firmware HAL must also implement reset/abort and timeouts.

```c
/* Inside the caller's application, with a supplied GpuTransport transport: */
Gpu gpu;
uint8_t caps[64], status[64], record[320];
GpuBounds bounds = {40, 60, 160, 120};
uint32_t target;

if (!gpu_start_session(&gpu, transport, 1000, caps, status)) {
    /* Inspect sideband faults, then recover the physical link. */
    return false;
}
target = 1u - status[14];

/* A complete blue clear: all 300 bins are empty. */
if (!gpu_submit_one(&gpu, target, 0x001f, NULL, bounds) ||
    !gpu_fence(&gpu, 100) || !gpu_present(&gpu, target, 101)) return false;
target = 1u - target;

/* Flat orange rectangle over a cleared black background. */
gpu_make_rect(record, bounds, 0x0080ff); /* R=255, G=128, B=0. */
if (!gpu_submit_one(&gpu, target, 0, record, bounds) ||
    !gpu_fence(&gpu, 102) || !gpu_present(&gpu, target, 103)) return false;
target = 1u - target;

/* A concrete prepared triangle, requiring no floating-point setup library. */
gpu_make_example_triangle(record);
bounds.x0 = 0; bounds.y0 = 0; bounds.x1 = 16; bounds.y1 = 16;
if (!gpu_submit_one(&gpu, target, 0, record, bounds) ||
    !gpu_fence(&gpu, 104) || !gpu_present(&gpu, target, 105)) return false;
target = 1u - target;
```

For a keyed sprite, upload a 4×4 RGB565 bitmap once, then reuse its heap address:

```c
uint8_t pixels[32]; /* 4 pixels * 2 bytes * 4 rows, pitch 8. */
unsigned i;
GpuBounds sprite_bounds = {100, 80, 104, 84};
for (i = 0; i < 16; ++i) gpu_put16(pixels + 2 * i, (i & 1) ? 0x07e0 : 0xf81f);
if (!gpu_upload(&gpu, GPU_TEXTURE_ADDR, pixels, sizeof(pixels)) ||
    !gpu_make_sprite(record, sprite_bounds, 0, 0, GPU_TEXTURE_ADDR, 4, 4, 8, true, 0xf81f) ||
    !gpu_submit_one(&gpu, target, 0, record, sprite_bounds) ||
    !gpu_fence(&gpu, 106) || !gpu_present(&gpu, target, 107)) return false;
target = 1u - target;
```

These snippets continue the same application, toggling `target` after each
successful presentation so it identifies the currently writable physical buffer.
The magenta key texels preserve the cleared background. The sprite is raw
replacement with depth disabled; it does not require new triangle setup.

A framebuffer copy uses a zero-initialized 56-byte payload. Continuing after
the successful sprite presentation above, this copies a 64×64 region from the
current front to the writable back, preserving other back-buffer contents:

```c
uint8_t copy[56] = {0};
uint32_t copy_sequence;
gpu_put32(copy + 4, 1u - target); /* source kind 0: current physical front */
gpu_put32(copy + 8, target);
gpu_put32(copy + 12, 20); gpu_put32(copy + 16, 30);  /* source X/Y */
gpu_put32(copy + 20, 200); gpu_put32(copy + 24, 100); /* destination X/Y */
gpu_put32(copy + 28, 64); gpu_put32(copy + 32, 64);   /* width/height */
if (!gpu_send(&gpu, GPU_COPY_RECT, copy, sizeof(copy), &copy_sequence) ||
    !gpu_fence(&gpu, 108) || !gpu_present(&gpu, target, 109)) return false;
target = 1u - target;
```

### Moving a sprite

There is no sprite-position register or persistent sprite overlay. After the
prior pass's fence, a driver may use `gpu_patch_bounds()` to update the four
destination coordinates, then update affected tile-table/list references and
submit a new pass. Keep width/height fixed for a pure move, preserve source
origin, and recheck source extent. Crossing a 32×32 tile boundary can require
different references. A bounds patch alone neither redraws nor presents.
This destination-bounds helper is suitable for rectangles and sprites. Moving
a triangle also changes its vertices, edges and attribute-plane setup; patching
only its bounds does not move its geometry.

For the small example, rebuild `record` with a new destination and call
`gpu_submit_one()` again: it regenerates the bins and clears the background,
so no old sprite trail remains. Reuse `GPU_TEXTURE_ADDR`; do not upload the
bitmap merely because its screen position changed. An efficient scene driver
can update a bounded dirty region, but must restore its old background and
maintain the real back buffer's contents/ownership. Active records are never
patched while the GPU is still using them.

### What these examples do not provide

The examples are packet/control code and small CPU-built records. They do not
provide an RP2350 GPIO/PIO program, electrical timing closure, a complete
allocator, a scene engine, a triangle setup port, or retained hardware sprites.
The repository's desktop host compiler is available in
[`protocol.hpp`](../reference/include/nano_gpu/hw/protocol.hpp) and
[`hw_protocol.cpp`](../reference/src/hw_protocol.cpp), and actual RTL bus driving,
reply handling and differential tests are in
[`reference/rtl/main.cpp`](../reference/rtl/main.cpp). Those implementations are
useful references, but their host language/runtime requirements must be ported
deliberately to the RP2350.

Run `make gpu-docs-test` to compile the marked block as strict C99, compare
packet bytes with the desktop encoder, and execute the examples through host
pins in the clocked C++ `SystemModel`. This checks the documented control code;
it is not a Verilator RTL-equivalence result or a board timing measurement.

## 10. Sources of truth when changing the interface

- [`nano_host_link.sv`](../rtl/gpu/nano_host_link.sv): wire-edge behavior, packet
  publication, capacities, sideband reads and parser faults.
- [`nano_gpu_core.sv`](../rtl/gpu/nano_gpu_core.sv): implemented commands,
  payload checks, capability/status bytes, sequencing, replies and recovery.
- [`nano_tile_renderer.sv`](../rtl/gpu/nano_tile_renderer.sv): primitive fields,
  supported modes and hardware-side record checks.
- [`protocol.hpp`](../reference/include/nano_gpu/hw/protocol.hpp) and its encoder:
  desktop serialization and validation reference.
- [GPU_ARCHITECTURE.md](GPU_ARCHITECTURE.md): supported feature set, memory and
  pipeline behavior that the control guide must describe consistently.

Change these two primary guides whenever a command, field, signal, ordering
rule, memory layout, supported feature or architecture changes. Update examples
and their checks with the implementation, then commit and push the guides with
that change. Older documents under `docs/` retain design history and detailed
derivations; where an M0 sketch differs from the implemented interface above,
this guide describes the current CPU contract.
