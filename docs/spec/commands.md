# M0 command and submission model

Profile: [N20K-M0-0.1](README.md). This specifies logical data, validation, and
ordering. Binary opcode numbers, primitive field offsets, FIFO capacities,
transfer timing, and pin assignments are M1 outputs. The provisional envelope
below is a design sketch, not a stable driver ABI.

## Transport sketch and completion model

Use an initial 8-bit data transport with backpressure and a readable response/
status path. A request is accepted only as a complete well-formed packet. A
partially received packet never executes. Packet sizes are bounded by reported
capabilities; long uploads are split into ordered packets. Never drop accepted
bytes when the FIFO is full. Physical clock and valid/ready timing remain open.

Provisional 16-byte request header:

| Byte offset | Type | Field |
| --- | --- | --- |
| 0 | 4 bytes | Magic bytes `4E 47 50 55` (`NGPU`) |
| 4 | u8 | Protocol major |
| 5 | u8 | Protocol minor |
| 6 | u16 LE | Opcode |
| 8 | u32 LE | Payload byte length, excluding header |
| 12 | u32 LE | Request sequence |

The packed ABI will define per-opcode payload sizes, padding, responses, and any
link integrity check. A wrong length/version or loss of framing stops parsing;
do not search for magic inside arbitrary texture payload. Out-of-band recovery
establishes a fresh packet boundary. Limits/versions are discoverable before use.

All mutating requests enter one ordered execution stream. Acceptance means
queued, not finished. Replies and status identify their request sequence and
execution epoch so old completions cannot be mistaken for work after reset or
abort. Completion identity is `(epoch, request_sequence, host_token)`, not token
alone. Request sequences MUST NOT repeat within an epoch. Before sequence wrap,
drain work, quiesce transport, and establish a new epoch with cleared completion
validity. The M1 bootstrap/recovery handshake must distinguish sessions across
reset/power cycles and require the host to discard stale local replies.
Status/capability reads and out-of-band recovery cannot be blocked behind a pass
or a presentation waiting for vertical blank.

## Logical commands

| Command | Required inputs | Behavior/completion |
| --- | --- | --- |
| `GET_CAPS` | Query sequence | Read-only snapshot of supported profile, limits, formats, operations, tile dimensions, memory layout, and numeric quality contract. Does not imply earlier mutations completed. |
| `GET_STATUS` | Query sequence | Read-only coherent status snapshot, including faults, ownership, and completion tokens. |
| `WRITE_HEAP` | Destination byte address, byte length, data | Ordered copy to a valid heap extent; affected metadata/texture data is visible to later commands before completion. Attachment writes through this command are forbidden. |
| `SUBMIT_PASS` | Pass-descriptor byte address | Validate descriptor, acquire resources, execute all 300 bins and attachment operations, then retire working state and complete stores. |
| `COPY_RECT` | Source view, target physical color ID, source/destination coordinates, width/height | Ordered RGB565 color copy outside a pass, with overlap semantics defined below. Depth is unchanged. |
| `FENCE` | Host token | Completes only after every earlier mutating command and required writeback completes. May be polled through status. |
| `PRESENT` | Physical color ID, host presentation token | Validate writable initialized target, seal it, swap at the next eligible vertical blank, then complete and release old front. |
| `READ_MEMORY` | Byte address/length, result token | Ordered diagnostic readback of a bounded SDRAM range after earlier stores; reads no hidden dirty tile data. Result transport/backpressure is finalized in M1. |
| Out-of-band `ABORT` | Recovery request | Drain issued operations, cancel pending stream work, release resources, report completion in a new epoch, and preserve valid front. |
| Out-of-band hard reset | Reset request | Restart parser/device initialization and epoch; no preservation guarantee for heap contents. |

An execution fence immediately before `PRESENT` means rendering is complete; a
fence after it cannot complete until the swap occurs. Read-only queries may
overtake the execution stream and therefore report older completion values.
Readback is intended for diagnostics and validation, not continuous scanout.
Abort/parser recovery requires quiescing host DMA and discarding old queued and
partial input before the fresh-boundary handshake completes. Epoch changes alone
do not make late old DMA bytes safe to parse as new packets.

## Pass descriptor

Required logical fields:

- Profile/record version and descriptor byte extent.
- Pass identifier for diagnostics, physical target color ID, and tile-table byte
  address. Tile count is exactly 300 for this profile.
- Independent color/depth load operations: `LOAD=0`, `CLEAR=1`.
- RGB565 clear color, D24 clear depth, and zero reserved bits.

The descriptor, table, lists, primitive records, and referenced textures MUST be
in valid non-overlapping live heap allocations. Each zero-count list has address
zero. All nonempty list/record addresses are 4-byte aligned. A record cannot
redirect or append work: no recursive lists, GPU-generated commands, or jumps.
Limits on total references and record fetches are reported by `GET_CAPS`; running
out of host-allocated list storage is a driver error, not permission to drop bins.
Allocation liveness/non-overlap is the driver's responsibility; GPU checks are
bounded structural/address/state validation rather than a host-allocator audit.

Attachment operations are global to the pass, not selectable per bin. All tiles
are logically visited, even when every reference count is zero. Physical target
ID is checked at execution rather than inferred from an earlier front/back query.

## Primitive records

Every record has a type, version, byte extent, integer half-open scissor, and
render state. Bounds and scissors are intersected with the viewport and active
tile. Reserved fields are zero. Records are immutable through pass completion.
There is no implicit mutable global render state: records carry the state needed
to interpret them. Driver helper defaults are full-viewport scissor and color
writes enabled; triangles default to LESS with depth test/write enabled, while
2D records default to depth test/write disabled. Enables are explicit in records.

Common render state comprises color-write enable, depth-test enable, depth-write
enable, comparison (`LESS`, `LEQUAL`, or `ALWAYS`), and material state appropriate
to the record. Disabled depth testing makes the comparison pass; depth writes
remain independently controlled. A fully rejected/keyed fragment writes neither
attachment. A record with both writes disabled is a valid no-op.

| Record | Required logical fields |
| --- | --- |
| `TRIANGLE` | Q4 snapped vertices for validation/reference, positive-area unbiased edge equations, clipped integer bounding box and its sample anchor, needed s64 attribute seeds/gradients, shading mode, constant RGB888 color or RGB planes, depth plane when used, and texture state when used. |
| `RECT` | Integer half-open destination bounds, constant RGB888 color, and constant D24 when depth is used. A pixel is a 1×1 rectangle. |
| `LINE` | Two integer pixel endpoints, constant RGB888 color, and constant D24 when depth is used. Coverage follows the global line rule, not a separately restarted line per tile. |
| `SPRITE` | Integer destination bounds, integer source origin, RGB565 texture state, optional color-key value/enable, and constant D24 when depth is used. Initial sampling is 1:1, unscaled and unflipped, with no color modulation. |

Triangle shading modes are `FLAT`, `GOURAUD`, `TEXTURE_REPLACE`,
`TEXTURE_FLAT_MODULATE`, and `TEXTURE_GOURAUD_MODULATE`. Color-key rejection is
available for textured triangles as well as sprites. A flat depth-only triangle
does not need texture or RGB planes. If keying is enabled, however, its texture
state and UV planes are required even when color writes are disabled: depth
updates still depend on the sampled key. Presence masks and packing remain M1
decisions. Canonical records retain snapped vertices for reference; whether
redundant validation-only vertices are transmitted is also a packing decision.

Texture state contains heap byte base, extent, power-of-two width/height in
`1..1024`, 4-byte-aligned row pitch at least `2*width`, RGB565 format, independent
U/V addressing (`CLAMP` or `REPEAT`), and optional raw RGB565 key. Declared extent
MUST be at least `pitch*height`, including final-row padding, and stay within
the heap. Fetches/burst refills MUST NOT overrun it. A one-texel-wide row
therefore has at least four bytes of pitch. Other texture formats are errors in
this profile, rather than implicit conversions.

Sprite source rectangles MUST lie within their texture. Source width/height
equal the unclipped destination width/height. Clipping the destination advances
the source coordinate by the same pixel offset; it does not stretch or restart
sampling. Texture address modes do not relax sprite source-extent validation.

## Copy rectangles

Source view is either physical color 0/1 (fixed dimensions/pitch) or a bounded
heap RGB565 image (explicit base, extent, width, height, pitch). Target is an
explicit physical color ID, checked to be writable at execution. Negative
width/height are errors; zero area is a no-op. Coordinates use signed integers.
Heap source-view dimensions MUST be in `1..1024` (not necessarily powers of
two), base and pitch 4-byte aligned, and `pitch>=2*image_width`. Extent MUST cover
`pitch*image_height` with checked wide arithmetic and remain entirely in the heap.
Copy width/height are separate from source-view dimensions. Full image/view
validation applies before clipping the copy request.

Parameterize the requested copy by integer offsets `0<=i<width, 0<=j<height`.
Retain only offsets for which both `(src_x+i,src_y+j)` and
`(dst_x+i,dst_y+j)` are in their respective image bounds. For each retained
offset, destination receives the source value from the start of this command.
Clipping never changes source/destination correspondence. No depth, filtering,
keying, or color modulation is applied by `COPY_RECT`.

The GPU captures physical source/target identities for the command; no presentation
occurs during the copy. Heap source allocations remain immutable while copying.
A front-buffer source is permitted because scanout is read-only. A front-buffer
destination is always an ownership fault. Full-color copies initialize an invalid
back buffer only if they cover every destination pixel; partial copies do not.

## Status, counters, and fault reporting

Logical status MUST expose:

- Ready/busy/presentation-pending/fault/abort-complete state, execution epoch,
  accepted request sequence, and last successfully completed fence/presentation
  `(epoch, request_sequence, token)` identities with validity flags.
- Current front color ID, writable back ID when available, and back/depth
  initialization-valid flags. A sealed/presentation-waiting buffer is not
  writable. A merely queued PRESENT does not retroactively change ownership for
  earlier commands; sealing occurs when PRESENT reaches execution.
- Sticky fault code and failing request sequence; pass/tile/record address when
  applicable. Faults suppress later success notifications until recovery.
- Coherent counter snapshots for cycles, covered fragments, depth rejects,
  keyed rejects, committed color writes, texture misses, memory stalls, and
  scanout underflows, as those stages are implemented. Capabilities identify
  available counters. Counter width/wrap behavior is finalized in the ABI.

Initial fault categories: malformed packet/version/length, unsupported
opcode/feature, bad alignment/range/extent, illegal ownership, invalid attachment
load, invalid descriptor/state, numeric overflow/domain, and device memory fault.
Descriptor errors discovered after work begins follow the partial-pass recovery
rules in [architecture.md](architecture.md). Malformed transport needs parser
recovery as well; recovery never interprets bytes inside upload data as commands.

## Example logical frame

```text
GET_CAPS / GET_STATUS -> determine epoch and writable physical target
WRITE_HEAP(texture data)
WRITE_HEAP(primitive records, ordered lists, 300-entry table, pass descriptor)
SUBMIT_PASS(target=back, color=CLEAR black, depth=CLEAR 0xFFFFFF)
SUBMIT_PASS(target=back, color=LOAD, depth=LOAD, bins containing 2D overlay)
FENCE(rendered_token)
PRESENT(target=back, presentation_token)
FENCE(presented_token)
```

These are symbolic operations, not serialized bytes. The driver can queue them
without blocking after each command. If it queues the next frame before reading
status, it must use the known physical buffer alternation and stream order;
the execution-time ownership check still applies. Resident textures can be reused
without uploading every frame. Metadata can be rewritten by later ordered
`WRITE_HEAP` requests once the previous pass has executed.
The second pass illustrates LOAD preservation; ordinary overlays can instead be
appended to each first-pass tile list to reduce attachment traffic.
