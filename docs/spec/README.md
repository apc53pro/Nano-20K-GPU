# M0 specification — N20K-M0-0.1

Status: initial architecture specification, 2026-10-23. **Tiled rendering is a
confirmed project decision.** The remaining choices below define the initial
engineering contract for the reference model and feasibility prototypes.

MUST and MUST NOT describe required behavior for this profile. SHOULD identifies
a recommendation. M1 may revise this version explicitly when measurements expose
a cost or timing problem; implementations must not silently change its rules.
The logical contract is specified here. A stable packed binary ABI and electrical
bus timing are not yet released.

| Decision | Initial contract |
| --- | --- |
| Display | Native 640×480 RGB565; approximately 60 Hz, video-only DVI-compatible TMDS through HDMI. |
| Rendering | CPU transform/clip/setup/binning; GPU tiled rasterization, interpolation, shading, depth, and 2D drawing. |
| Logical tiles | 32×32 pixels; 20 columns × 15 rows = 300 tiles. |
| Attachments | Two RGB565 color buffers; one shared D24 depth buffer stored in 32-bit words. |
| Submission | Immutable primitive records stored once; ordered per-tile reference lists; one pass executes at a time. |
| Coordinates | Screen Y down; Q4 subpixel vertices; pixel-center sampling; top-left coverage. |
| Arithmetic | CPU prepares bounded signed 64-bit attribute planes; depth/color rules are exact; perspective reconstruction has an explicit error budget. |
| Textures | RGB565, power-of-two dimensions from 1 to 1024, nearest sampling, clamp or repeat. |
| Shading | Flat RGB, Gouraud RGB, texture replacement, and flat/Gouraud texture modulation. |
| 2D | Pixels/rectangles, lines, 1:1 color-key sprites, and ordered RGB565 copies. |
| Control | Versioned logical commands over an initial 8-bit transport with backpressure and independent status/recovery access. |

- [Architecture and memory](architecture.md): tile lifecycle, attachments, heap,
  ownership, ordering, presentation, and recovery.
- [Rasterization and numeric rules](rasterization.md): CPU setup, exact coverage,
  interpolation, texture/color/depth behavior, and 2D coverage.
- [Command model](commands.md): logical records, commands, visibility, status,
  validation, and the provisional transport envelope.
- [Validation contract](validation.md): conformance rules, hand-checked vectors,
  benchmarks, and remaining M0 implementation work.
- [Golden vector data](golden-vectors.json): small inputs/expected results for the
  executable software conformance suite; RTL validation remains future work.

The parent [requirements](../requirements.md) remain the product goals; this
profile supplies concrete initial details. Optional indexed textures, dithering,
scaling/flipping, fog, stencil, blending, filtering, and GPU setup are outside
this profile. Capability bits must not advertise an unimplemented feature.

M1 still determines physical tile banking, number of resident tiles, texture
cache size, pipeline stages, reciprocal implementation, clocks, pin assignments,
and the packed record/command ABI. A physical design may process a logical tile
in smaller internal blocks if results and ordering remain identical.

The specification portion of M0 and the first [software reference
implementation](../software-reference.md) are delivered. The implementation
includes CPU setup, canonical fixed-plane rendering, tiled and monolithic
traversals, executable conformance tests, demo scenes, and image/depth exports.
Independent geometric error analysis, implementation-reciprocal checks, and
captured command traces remain work before the complete M0 validation is finished.
