# Selected GPU storage mapping

Profile: `N20K-STORAGE-0.3`. The executable allocation is in
[storage_map.hpp](../../reference/include/nano_gpu/hw/storage_map.hpp).
The system now selects **39 of the Tang Nano 20K's 46 physical BSRAM blocks**.
The mapping uses the same SDPB 1024x16 primitive contract as the tile and cache;
a logical 32-bit memory combines two instances at the same address.

This is a storage and port selection, not a synthesis result. Native memory
port delays are represented by the model. LUT/FF packing, combinational timing,
DSP packing, placement and routing remain to be qualified. Reports distinguish
`mapping_selected=true` from `physical_fit_validated=false`.

## Native memories

Every listed bank occupies a complete 18,432-bit Gowin BSRAM block. The chosen
x16 configuration exposes 16,384 data bits per block; remaining bits do not
provide extra data capacity or ports.

| Logical allocation | Instances | Native banks | Used words per instance | Meaningful bits per word |
| --- | ---: | ---: | ---: | ---: |
| Resident tile RGB565 | 1 | 1 | 1024 | 16 |
| Resident tile depth low | 1 | 1 | 1024 | 16 |
| Resident tile depth high | 1 | 1 | 1024 | 8 |
| Texture cache / copy source row | 1 | 1 | 1024 | 16 |
| Scanout FIFO | 2 | 2 | 1024 | 16 |
| Host RX FIFO | 1 | 1 | 256 | 8 |
| Host TX FIFO | 1 | 1 | 1024 | 8 |
| Host packet payload | 3 | 3 | 1024 | 8 |
| Reply serializer | 1 | 1 | Up to 1024 | 8 |
| SDRAM client payload slot | 6 | 12 | 16 | 32 |
| Shared record/readback reader | 1 | 2 | 129, including alignment guard | 32 |
| Completed-tile / LOAD-row color staging | 1 | 1 | 1024 | 16 |
| Completed-tile / LOAD-row depth-low staging | 1 | 1 | 1024 | 16 |
| Completed-tile / LOAD-row depth-high staging | 1 | 1 | 1024 | 8 |
| Fragment commit address and depth | 1 | 3 | 64 | 34 |
| Fragment RGB factor | 1 | 2 | 64 | 24 |
| Fragment RGB565 result | 1 | 1 | 64 | 16 |
| Reciprocal seed base | 1 | 3 | 256 | 35 |
| Reciprocal seed gradient | 1 | 1 | 256 | 16 |
| **Total** | | **39** | | |

The previous streamed-storage system selected twenty-nine banks. Six context
banks and four reciprocal-seed banks add ten. The resident renderer now selects
fourteen banks; the complete system has seven physical banks left unallocated.
The DSP allocation is 46 of 48 native 18×18 equivalents: one setup, forty
perspective, two texture-address and three RGB modulation units.

The selected banks occupy **718,848 physical bits**, exposing **638,976 x16
data bits**. Logical payload capacities use **199,040 meaningful bits**, plus
**32 alignment-guard bits**, for **199,072 reserved bits**;
**439,904 selected data bits remain unused**. The existing three staging banks
now use all 1024 addresses to retain a completed tile; their additional 39,680
meaningful bits consume previously unused capacity, without allocating another
native bank. Independent port ownership and
narrow fields take precedence over treating spare capacity as free parallel
storage. FIFO stress limits do not release a physical bank.

## Common port contract

One SDPB instance permits one full-word write and one synchronous read on an
edge. The model forbids simultaneous read/write at the same address. Each x32
pair supplies one 32-bit access at one address, not two independent ports.

A read request is applied on edge N. Its output becomes established after that
edge and can be captured by a consumer on edge N+1. The consumer must inspect
the previous output before the next native memory tick. No controller may use
a result newly produced later on its own common edge. Disabled accesses retain
the output latch; resetting the output does not clear the memory contents.

## SDRAM client slots

Host, Command, Tile, Texture, Copy and Scanout each own one two-bank slot. A slot
holds at most sixteen 32-bit words. Its data storage is reused for write-request
payload or read-response payload; both are never live simultaneously for that
client. Four byte-enable bits per word are separate registers.

The system interface uses `write_request_word()` and `submit_prepared()` for
writes. Completed metadata comes from `response_info()`; payload is consumed
with `request_response_word()`, `response_word_valid()` and `response_word()`.
`release_response()` retires ownership. These interfaces are declared in
[sdram.hpp](../../reference/include/nano_gpu/hw/sdram.hpp).

Write payload enters through at most one 32-bit word port per core edge. Queue
publication follows payload preparation. The SDRAM write engine reads the
selected slot through its synchronous port and prefetches data before emitting
the corresponding physical DQ beat. A read transaction deposits returned DQ
words through that slot's write port.

A completed response retains its slot until the client has drained it and
explicitly releases ownership. Response metadata can be observed without
copying the payload. Payload consumption requires synchronous word reads.
Texture/cache, scanout and copy consumers split each 32-bit word into two
16-bit items on separate write edges. Short texture lines zero-fill their
unused cache tail without issuing out-of-range SDRAM reads.

Whole-array request/response values used by isolated tests or diagnostics are
host adapters. They do not supply an additional GPU data port. The system
execution paths use the slot ports; an array snapshot cannot replace a
synchronous read or bypass slot ownership.

## Reader, lookahead and tile staging

The reader reserves 129 x32 words in two native banks for a payload of at most
512 bytes. An unaligned 512-byte `READ_MEMORY` spans as many as 516 bytes of
aligned words; the extra word carries alignment guard bytes, not additional
payload capacity. Fetched words enter its write port serially. The
controller-slot read is pipelined: capturing an established word can issue the
next synchronous read on that same edge. Descriptor, tile-entry, reference and
primitive fields capture that same established word into their bounded register
bank while it enters reader RAM; they do not require another serial RAM-read
pass. The largest record is 320 bytes, or eighty 32-bit words.

Primitive fields are serially captured into a distinct, bounded decoded-field
register bank with a 320-byte upper bound. The decoded C++ object is a view of
those fields, not another payload RAM or an additional parallel read port.
The tile command-capture edge then establishes its own immutable primitive
registers. While that primitive draws, the same reader and field registers can
fetch one following reference and record. The next primitive still waits for
the current one to drain before capture. No second record bank or parallel
record-read port is added. Both retained sets of fields must be accounted for;
they are not silently aliased to the reader RAM. Readback does not need a 512-byte FF copy:
the reply path streams reader words through a single-word hold into the reply
serializer.

The staging banks have the same three-way width split as the resident tile.
For a pass clearing both attachments, one control edge exchanges the resident
and staging bank-set roles. A bank-select mux redirects their native ports;
the 1024 pixels are not copied or read in parallel. The renderer initializes
and draws the next tile in the other bank set while the completed set exports
to SDRAM. Roles cannot exchange again until the previous export finishes, and
pass completion waits for the final write acknowledgement. Loading either
attachment retains the serialized load/draw/store schedule and uses the
staging-role banks for row staging.

The tile writer consumes complete 32-bit words. Color packing reads two RGB565
pixels on separate native read edges; depth reads the low/high banks together
for one D24/U32 word. A fixed 32-bit output hold and 16-bit color-half register
bridge synchronous reads and client-slot fills. There is at most one read per
bank per edge, and simultaneous read/write at the same staging address remains
forbidden. Host uploads retain byte packing because their source is the
eight-bit packet port. No row-serialization array or duplicate sixteen-word
writer payload is allocated.

## Fragment contexts and reciprocal ROM

Textured draws retain up to sixty-four ordered fragment contexts in six native
banks. Untextured draws retain the existing one-pending-fragment fast path. Three retain the
commit tile address (10 bits) and D24 depth, two retain the RGB888 modulation
factor, and one holds the eventual RGB565 result. These are separate memories:
commit metadata can be read for retirement while factors feed shading and a
completed color writes the result bank. Each still has only one read and one
write port. Synchronous context reads cross a distinct capture edge.

Ready state is sixty-four one-bit registers; each context has a two-bit
retirement reason (commit, depth rejection or key rejection). Ring head/tail
and count establish validity. Result writes become visible before readiness can
permit a dependent retirement read. Old context addresses are not recycled until
ordered retirement releases their ownership. Pipeline context tags are six bits;
trace fragment IDs remain host observations.

Early depth reads occur at acceptance and compare on the following edge before
UV work. Every currently supported walker visits each pixel at most once within
a primitive, and primitives drain before another begins. Therefore no older
in-flight fragment can update that same address before its early test. The
selected implementation does not retest depth at retirement. A future design
which overlaps primitives must add a depth-hazard policy instead of assuming
this property continues to hold.

The shared-reciprocal pipeline uses a fixed 256-entry linear seed ROM: a 35-bit
base spans three native banks and a 16-bit gradient occupies a fourth. ROM words
are initialized through selected BSRAM INIT configuration and use synchronous
read ports; resetting outputs preserves those contents. Every wide arithmetic
product is assembled from registered native 18×18 partial products. See the
[arithmetic design and proof](reciprocal-pipeline.md).

## Register and logic-ROM selection

The following fixed arrays deliberately remain registers. Their aggregate is
**6,404 bits**; this is a subtotal, not a whole-device FF estimate.

| Register allocation | Selected bits | Access |
| --- | ---: | --- |
| Current sideband status | 512 | Parallel status register view |
| Atomic status snapshot | 512 | Parallel capture on accepted register-1 read |
| Packet parser header | 128 | One byte capture at a time |
| Executor captured command fields | 512 | Clocked packet-bank byte reads |
| Decoded primitive-field bank | At most 2560 | Streamed reader word capture |
| Reply envelope and source snapshot | 736 | Fixed 92-byte source, distinct from host status snapshot and reply BSRAM |
| SDRAM byte masks | 384 | Six clients x sixteen words x four lanes |
| Writer packing word and mask | 36 | Byte-packed host source or complete tile word |
| Texture tags | 768 | Sixty-four 12-bit tags |
| Texture valid bits | 64 | Per-line state; changed binding or a newly uploaded pass invalidates tags |
| Context ready state | 64 | One ready flag per retained fragment |
| Context retirement reasons | 128 | Two bits per retained fragment |

The immutable 64-byte capabilities bank is **512 bits of logic ROM**, selected
by its byte address. It does not allocate another BSRAM or require 512 mutable
flip-flops. Configuration fields which vary between simulation runs are fixed
for the selected hardware build.

The reply source can retain a 28-byte envelope and 64-byte status/capability
snapshot while the serializer writes its native bank one byte per edge. It is
separate from the live sideband and register-1 snapshot, so a concurrent host
status read cannot change an in-progress reply. `READ_MEMORY` retains only its
reply prefix there and streams up to 512 payload bytes from the reader RAM;
it does not extend that source into a 512-byte register array.

Each SDRAM slot also has sixteen written-word flags (96 bits across all
clients), one pending input word/address/mask/valid register (246 bits across
clients), and pending output address/valid state (36 bits across clients).
Texture, scanout and copy each retain one 32-bit response word while consuming
its two halfwords; copy also has a 32-bit write word and four-bit mask.

Remaining scalar data and arithmetic state maps to bounded registers or the
selected DSP registers. The software host type's padding, pointer values and
container metadata do not describe hardware storage:

| State | Selected field widths and ownership |
| --- | --- |
| Immutable triangle | Six signed 32-bit Q4 coordinates; three signed 32-bit `{a,b,c}` edge triplets and three inclusion bits; signed 32-bit bounds; seven `{seed,step_x,step_y}` signed 64-bit planes; bounded draw state |
| Tile plane working set | Seven signed 64-bit plane triplets plus seven row and seven current signed 64-bit accumulators; seven enable bits |
| Tile edge working set | Three signed 32-bit values in each of row, current, X-step and Y-step sets |
| Draw state | RGB888 factor (24 bits); RGB565 key (16 bits) plus enable; depth function (2 bits); shade mode (3 bits); address modes (1 bit each); attachment-enable bits; signed 32-bit scissor fields; texture descriptor fields (32 bits each) |
| Pending fast fragment | Address10, RGB56516, D24 and forwarding D24, valid/test/write/forward flags and depth function2; untextured next-edge retirement |
| Fragment scheduling | Six-bit context tags and ring selectors; seven-bit occupancy count; bounded depth-read, UV, texture and retirement handoff state; payloads reside in the six context memories |
| Perspective pipeline | Fixed-width normalized q, seed/refinement/correction registers and six-bit context tags across 26 enabled stages; forty registered native multipliers; no serial-divider state in the selected path |
| RGB pipeline | Three parallel registered native products plus bounded factor/color/context-tag metadata and exact rounded divide-by-255 logic |
| Multiplier | Two signed 18-bit operand registers and two 36-bit product/output registers within the selected registered DSP configuration |
| 2D walker | Signed 32-bit command/window and source fields; visible X/Y counters within 640x480; signed 16-bit Bresenham differences, increments and error |
| Texture control | At most eight input/address/tag/read/result reservations, two registered address DSPs, one external miss, one held SDRAM word; no line-response array |
| Copy control | Bounded source/destination row addresses, pitch, shift/add state, pixel/word/lane counters, request metadata and one-word streaming state; full source row stays in the shared cache bank |
| SDRAM control | Per-client address (23 bits), word count (5 bits), tag (32 bits), byte masks and ownership bits; four open-row tags (11 bits each) plus timing/state counters; one active transaction |
| Host control | Three packet metadata entries, two queue selectors, FIFO pointers/occupancy, parser progress, byte-hold/setup/hold state and registered byte outputs |
| Scanout control | FIFO pointers/occupancy; pixel position and divider state; front-buffer generation/tag and outstanding request; registered output pixel and one-word streaming state |
| System control | One ordered command/pass/copy descriptor, tile/reference/row counters, front/back validity and seal bits, epoch/sequence/fence/present tokens and recovery state |
| Record lookahead | One 32-bit record address, two-bit prefetch state and one invalid-address flag; record payload reuses the existing reader and decoded-field bank |
| Tile export control | Bank-role selector and native-port mux; three-bit export state, five-bit row index, two 23-bit output base addresses; 32-bit word hold, 16-bit color half, five-bit pending read/word indices, six-bit next index, half selector and bounded valid/pending/depth/staging flags |
| Optimization counters | Eight 64-bit counters: prefetched records, record-fetch overlap/wait cycles, tile-export active/overlap/wait cycles, published tile pixels and bank-role exchanges; 512 additional scalar counter bits |

No blanket alias is assumed between simultaneously live fields. Small address,
count and state fields can be narrowed only within the documented legal input
domain. Full LUT/FF accounting must include muxes, comparators, adders, reset
distribution and the retained fields above. The register subtotal and BSRAM
capacity checks therefore do not establish timing closure or full-device fit.

## Host observations

Framebuffer snapshots, captured display images, poison/defined-byte tracking,
diagnostic payload mirrors, trace callbacks, fragment IDs, CSV/JSON streams and
benchmark scoreboards are host observations. They do not create GPU memory
ports or FPGA trace buffers. Simulation-only geometric assertions also remain
outside the timed datapath.

Statistics used only for reporting are host counters. Any counter exposed as a
live hardware status register or used for device control still requires an
explicit bounded register implementation; observation does not provide a free
control signal.

See [the system model](system-model.md) for command/memory sequencing,
[the hardware profile](gowin-profile.md) for vendor primitive choices, and
[the block interfaces](block-interfaces.md) for rendering arithmetic.
