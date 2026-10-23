# M0 conformance and validation contract

Profile: [N20K-M0-0.1](README.md). The [software reference
renderer](../software-reference.md) implements canonical CPU setup, fixed-plane
rendering, tile/monolithic comparisons, and executable conformance cases. The
suite consumes the small data cases in [golden-vectors.json](golden-vectors.json).
The broader validation requirements below also cover future arithmetic, command
transport, and hardware behavior; those parts are not yet implemented.

## Reference layers

The reference work must distinguish three layers:

1. **Geometric reference:** high-precision clipping, projection, and attributes.
   Quantify Q4 snapping separately; do not treat a pixel difference at a moved
   geometric edge as reciprocal error.
2. **Canonical setup/fixed-plane reference:** exact integer edges, rational RNE
   setup, s64 plane evaluation, depth tests, color operations, and tile/list
   ordering. Evaluate triangles monolithically as well as by tiles.
3. **Implementation arithmetic:** the chosen reciprocal pipeline and any narrowing
   must meet the reconstruction bound. Its results must be deterministic for a
   given sample regardless of cache misses, stalls, or tile partitioning.

Coverage, fixed-plane depth conversion, depth comparison/update, 2D coverage,
color packing/modulation, copy semantics, and command ownership/order are exact.
UV reconstruction has the explicit tolerance in rasterization.md. The current
software model reconstructs UV by exact integer division into Q16 coordinates.
An implementation-arithmetic mode must be added when M1 selects the reciprocal
pipeline. The monolithic mode independently checks traversal, but shares setup
and shading arithmetic; it is not an independent geometric oracle.

## Texture-boundary acceptance

Nearest sampling is discontinuous. Tiny UV differences near an integer texel
boundary can change color substantially, and a transparent-index/key boundary
can also change depth updates. A blanket RGB tolerance is not valid.

For exact quantized-plane ratio u, let `epsilon=1/1024` texel. The chosen sample
index MUST be in `{floor(t): u-epsilon <= t <= u+epsilon}`, followed by the
specified clamp/repeat mode; apply the same rule independently to v. If u is
more than epsilon from every integer boundary, its index MUST match the exact
reference. For comparison to the ideal snapped-geometry ratio rather than the
quantized plane, use the larger end-to-end bound of 1/256 texel.

For each implementation-selected texel, key rejection, shading, and depth/color
commits must follow the exact rules. Inside a permitted boundary band, resulting
visibility differences are evaluated from that selected texel, not dismissed
as arbitrary image error. The implementation must choose deterministically and
must not change its choice when a triangle crosses tiles or experiences stalls.

## Small golden cases

| ID | Input | Expected result |
| --- | --- | --- |
| COVER-01 | Triangle `(0,0),(2,0),(0,2)` | Only pixel `(0,0)` is covered. |
| COVER-02 | Adjacent triangle `(2,0),(2,2),(0,2)` | Pixels `(1,0),(0,1),(1,1)` are covered; together the two triangles own every 2×2 square pixel exactly once. |
| TILE-01 | Translate that square by `(31,31)` | Exactly `(31,31),(32,31),(31,32),(32,32)`, belonging to tile IDs 0, 1, 20, 21. |
| DEPTH-01 | LESS: draw `0x800001`, then `0x800000`, then equality | Second draw replaces first; equal third draw fails. The first two have identical upper 16 bits, detecting a secretly 16-bit depth path. |
| DEPTH-02 | Clear `0xFFFFFF`; compare that value and `0xFFFFFE` | Exact clear/far value fails LESS; `0xFFFFFE` passes; equality then fails. |
| KEY-01 | Keyed near texel, then opaque farther texel | Keyed fragment writes nothing; farther fragment passes unchanged background depth and becomes visible. |
| UV-01 | Triangle `(0,0),(4,0),(0,4)`; q=`(1/2,1/4,1/2)`, u=`(0,8,0)`, v=`(0,0,8)` | At `(0,0)`, q=`15/32`, U=`1/4`, V=`1/2`; reconstructed UV=`(8/15,16/15)` samples texel `(0,1)`. Affine interpolation would sample `(1,1)`. |
| LINE-01 | Endpoints `(0,0),(3,1)`, including reversed input | Pixels `(0,0),(1,0),(2,1),(3,1)`. |
| COLOR-01 | White texel modulated by RGB `(128,64,32)` | Output RGB565 `0x8204`; white modulation preserves every RGB565 source word. |

The JSON data includes the exact integer planes for UV-01. These values were
calculated from the specification; they provide implementation seeds rather
than exhaustive coverage or performance evidence.

## Required conformance cases

| Area | Cases and required observations |
| --- | --- |
| CPU setup/coverage | Both winding inputs after normalization, shared horizontal/vertical/sloped edges, all Q4 positions, degenerate and thin triangles, viewport/scissor borders, empty bins, and consistent clipping/refinement. No overflow or out-of-range writes. |
| Plane evaluation | Negative/tied RNE, large gradients, seeds outside covered area, maximum coordinates, tile/row starts, pauses, and traversal changes. Exact same accumulator at each sample as monolithic evaluation. |
| Depth | Constant values differing only in low bits, close interpolated surfaces, clear/far/equality, LESS/LEQUAL/ALWAYS, independent test/write enables, same-pixel hazards, and depth-only draws. |
| Tiles/passes | One/four/many tiles, empty-list clears, LOAD preservation, multiple passes, alternate tile orders, physical subtile variants, and shared-depth behavior across presentation. No tile-induced changes. |
| Textures/shading | Supported sizes/pitches, negative UV, exact/near boundaries, clamp/repeat, strong perspective, maximum supported W ratio, flat/Gouraud/replace/modulation, cache refill/eviction, and ordered texture updates. Meet numeric/address rules. |
| Keying | Raw key before modulation; depth-enabled keyed triangles/sprites; color-disabled depth-only keyed draws; a later farther opaque fragment. No provisional depth commit or skipped key lookup. |
| 2D/copies | Single pixels, empty/full/clipped rectangles, line reversal/ties/tile crossings, clipped sprite origins, overlap copies in each direction, and copy/render cache coherence. |
| Ownership/visibility | Immutable active resources, explicit target checks, queued uploads after passes, fence before/after presentation, front/back reuse, invalid LOAD, and delayed frames. No illegal writes or early completion. |
| Errors/recovery | Bad packet framing, lengths/versions, unsupported states, integer-wrapping addresses, malformed late tile records, abort during writes/passes/copies, and reset during presentation. Front remains valid on soft failure; invalid back/depth cannot be reused incorrectly. |
| Display | Active uploads/rendering, late frames, swaps at blank, startup/recovery black output, clock-domain/reset behavior, and recorded underflow count. |

Small golden cases should be supplemented with generated/property cases for
tile-versus-monolithic equivalence, shared-edge ownership, signed arithmetic,
address bounds, and deterministic backpressure behavior. Tests must exercise
observable correctness rather than merely duplicate an implementation formula.

## Benchmark definitions

Create parameterized scenes for:

- Large flat/textured triangles with coherent hits and cold/missing texture data.
- Many small triangles, tracking CPU setup/binning and record/reference traffic.
- Increasing overdraw, separating depth-rejected fragments from passing writes.
- Strong perspective and supported q/UV/gradient limits.
- 2D fills, directional overlap blits, sprites, and mixed UI/3D passes.
- Resident textures versus ongoing uploads, with normal scanout active.

Report profile/build, clocks, mode, triangle count, tile references, host bytes,
CPU transform/setup/bin time, covered fragments, depth/key rejects, committed
color writes, cache misses, stalls, frame time, and scanout underflows. Record
warm/cold cache conditions and the duration/traffic of stress runs.

The one-pixel-per-cycle preference is measured first as covered opaque textured
fragment initiation after pipeline fill, with ready local attachments, texture
hits, and no same-pixel dependency. Also report committed hit-case fill rate and
sustained scene performance, including misses, tile transfers, host cost, and
display traffic. Do not advertise candidate bounding-box positions as pixels.
No sustained scene/frame-rate guarantee or core clock is set by this M0 profile.

## Remaining work to finish the complete M0 milestone

Delivered: canonical CPU setup and rendering; exact golden cases; generated
mixed-primitive, shared-edge, numeric-domain, and tile-equivalence tests; three
parameterized demo scenes; and color/depth/image/statistics export. A checked
[showcase image](../images/software-reference.png) is included in the repository.

- Add an independent higher-precision geometric model and quantify clipping,
  projection, Q4 snapping, and fixed-plane error separately.
- Add approximate-UV conformance checks when M1 selects implementation arithmetic.
- Extend workload coverage for many small triangles and continuous uploads;
  capture reproducible primitive/bin traces and benchmark baselines.
- Exercise transport backpressure, asynchronous failures, cache coherence, and
  scanout on the corresponding future models and hardware.

The synchronous C++ API models completed passes, copies, and logical presentation.
Packet parsing, command FIFOs/fences, a shared SDRAM heap, cache behavior, and
cycle timing are outside this first implementation. Hardware acceptance is not
established by the software tests.
