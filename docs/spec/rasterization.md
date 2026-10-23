# M0 rasterization and numeric contract

Profile: [N20K-M0-0.1](README.md). This profile uses one sample per pixel, with no
antialiasing, blending, gamma conversion, or programmable shading. Equations are
mathematical definitions; they do not prescribe the pipeline implementation.

## Rounding and setup domain

`RNE(x)` rounds to nearest integer, with exact half-way cases choosing the even
integer. It is symmetric for negative values. `floor` and `ceil` have their
mathematical meanings; truncating a negative value toward zero is not floor.
Signed fixed-point values use two's complement. Shift/division implementations
must reproduce the stated rounding, including negative ties.

The CPU uses sufficient intermediate precision for setup (for example signed
128-bit integer arithmetic for the bounded integer plane formulas below). It
MUST check range before narrowing. An unrepresentable primitive must be refined
with consistent shared edges or reported as unsupported geometry before
submission. Silently wrapping, reducing depth precision, or dropping it is not
permitted. GPU detection of an invalid submitted numeric record is a fault.

For the canonical setup input, clip-space coordinates obey
`-w<=x<=w`, `-w<=y<=w`, `0<=z<=w`, and `w>0`. The CPU clips before division,
preserving shared clip vertices and associated attributes. Projection is:

```text
screen_x = 320 * (x/w + 1)
screen_y = 240 * (1 - y/w)
depth_normalized = z/w
```

Other application projections may be converted to this convention on the CPU.
Clip results to `[0,640] × [0,480]` before coordinate quantization. Shared
vertices MUST use identical snapped coordinates. Refinement cannot independently
snap shared-edge subdivisions and assume cracks/T-junctions will disappear.

The canonical clipper uses Sutherland-Hodgman polygon clipping in this order:
left `x+w>=0`, right `w-x>=0`, bottom `y+w>=0`, top `w-y>=0`, near `z>=0`,
far `w-z>=0`. Points on a plane are inside. A crossing edge with distances d0,d1
intersects at `t=d0/(d0-d1)`; interpolate clip x/y/z/w and raw u/v/RGB by that t,
before reciprocal-W construction. Remove consecutive identical output vertices.
Triangulate the resulting polygon as a fan from its first retained vertex,
preserving order, then snap and normalize each triangle as below. A remaining
vertex with nonpositive w is unsupported/singular, not a division target.
Exact geometry is the reference; an optimized CPU clipper must preserve the same
snapped shared vertices and canonical setup results for its supported inputs.

## Coordinates, orientation, and coverage

Screen X increases right and Y increases down. Vertex positions are signed
16-bit integers with four fractional bits: `X=RNE(16*screen_x)` and similarly Y.
Legal clipped values are `0<=X<=10240`, `0<=Y<=7680`.
Pixel `(x,y)` samples at integer subpixel point `(16*x+8,16*y+8)`.

For snapped vertices `P0,P1,P2`, compute integer doubled-area cross product
`D=(X1-X0)*(Y2-Y0)-(Y1-Y0)*(X2-X0)`. Zero-area triangles are culled by the CPU.
If D is negative, swap vertices 1 and 2 with all their attributes. Submitted
triangles MUST have D positive, which appears clockwise in the Y-down display.
Culling, if desired by the application, happens on the CPU before normalization.

For directed edge P0→P1:

```text
dx = X1-X0; dy = Y1-Y0
E(P) = dx*(Py-Y0) - dy*(Px-X0)
     = A*Px + B*Py + C
A = -dy; B = dx; C = dy*X0 - dx*Y0
inclusive = (dy < 0) or (dy == 0 and dx > 0)
bias = 0 if inclusive else -1
```

A sample is covered iff `E+bias >= 0` for all three edges and it is inside the
viewport, scissor, primitive bounds, and active tile. Coverage bias MUST NOT enter
attribute interpolation. Edge values and coefficients use signed 32-bit integers;
the clipped Q4 domain makes that sufficient. One pixel step adds `16*A` in X or
`16*B` in Y. Edge seeds MUST be evaluated from the original snapped geometry.

The conservative integer bounding box before scissor intersection is:

```text
x0 = max(0,   ceil((min_vertex_X - 8)/16))
y0 = max(0,   ceil((min_vertex_Y - 8)/16))
x1 = min(640, floor((max_vertex_X - 8)/16) + 1)
y1 = min(480, floor((max_vertex_Y - 8)/16) + 1)
```

Bounds are `[x0,x1) × [y0,y1)`. Intersect with integer half-open scissor bounds;
an empty result is a no-op and needs no bin references. The resulting minimum
pair `(x0,y0)` is the **sample anchor** (top-left visually): its subpixel
location is `(16*x0+8,16*y0+8)`. This anchor belongs to the whole primitive, not
each tile. It need not itself be covered.

## Attribute planes and guard precision

All needed attribute planes contain three signed 64-bit integers:
`seed`, `step_x`, and `step_y`. Values are evaluated as:

```text
F_acc(x,y) = seed + (x-x0)*step_x + (y-y0)*step_y
```

| Attribute | Integer scale: raw value / divisor | Vertex domain |
| --- | --- | --- |
| Projected depth in D24 code units | `2^16` | `[0,16777215]` code units before scaling |
| R, G, B channel code | `2^16` | `[0,255]` |
| Normalized reciprocal W, q | `2^37` | `[1/128,1/2]` before coefficient quantization |
| U=u*q, V=v*q | `2^27` | Texel-edge coordinates `u,v ∈ [-1024,1024]` |

Vertex attribute values are first quantized to their stated scale with RNE.
For depth, quantize `16777215*depth_normalized*2^16` directly; do not first reduce
the normalized depth to 16 or 24 bits. RGB planes are screen-affine Gouraud
values. Flat colors and 2D constant depth do not need interpolated planes.

Use these exact rational setup equations on quantized integer vertex values Fi:

```text
step_x = RNE(16*((F1-F0)*(Y2-Y0) - (F2-F0)*(Y1-Y0))/D)
step_y = RNE(16*((X1-X0)*(F2-F0) - (X2-X0)*(F1-F0))/D)
seed   = RNE((F0*E12(anchor) + F1*E20(anchor) + F2*E01(anchor))/D)
```

E12, E20, E01 are unbiased edges. Coefficients are rounded once. All tile/row
starts MUST be derived from that same integer plane without per-tile rounding.
Stalls and scan direction cannot change the result. Extra internal precision is
allowed only if it reproduces the specified integer plane values at samples.

For every plane of a nonempty primitive, the CPU MUST preflight the bound
`abs(seed)+(x1-x0-1)*abs(step_x)+(y1-y0-1)*abs(step_y) <= 2^63-1`.
Evaluate the bound with wide arithmetic, not signed-64 overflow. The seed,
gradients, and stepping expressions must fit signed 64 bits. This deliberately
rejects some poorly conditioned thin primitives until they are refined. A GPU
must detect arithmetic overflow rather than treating it as valid wrapped data.

## Perspective correction and its supported domain

For each post-clip triangle, define `r_i=1/w_i` and a single scale
`k=1/(2*max(r_i))`. Require `max(w_i)/min(w_i) <= 64` in this initial profile.
Then `q_i=k*r_i` lies in `[1/128,1/2]`. Set `U_i=u_i*q_i` and `V_i=v_i*q_i`,
using exactly the same k. Large W ratios require consistent CPU refinement or
an explicit unsupported-geometry result. Normalization does not change U/q.
For REPEAT, the CPU may first rebase all three U or V coordinates by the same
integer multiple of the corresponding texture dimension to enter the supported
UV domain; this preserves periodic sampling. Rebasing individual vertices by
different periods would change interpolation and is forbidden.

At a covered sample, evaluate the three integer planes, recover their real-scale
values, and reconstruct `u=U/q`, `v=V/q`. The quantized q MUST remain in
`[1/256,1]`; there is no divide-by-zero clamp or affine fallback. Interpolated
projected depth does not receive this division.

The reconstruction stage, including input narrowing, reciprocal/multiply error,
and final rounding, MUST be within **1/1024 texel per component** of the exact
ratio of the quantized planes. Its output uses signed 32-bit texel coordinates
with 16 fractional bits. The reciprocal algorithm/latency is an M1 choice;
canonical high-precision division supplies a reference value, not a mandated
hardware divider. Reconstructed values may exceed the vertex UV domain slightly
within the allowed error; address modes handle boundary crossings normally.

For at most 639 X and 479 Y increments, vertex/seed/gradient quantization
contributes at most 560 raw quanta per attribute. Consequently:

- q error is at most `560/2^37`, approximately `4.075e-9`.
- U/V error is at most `560/2^27`, approximately `4.173e-6`.
- Using conservative ideal q ≥ 1/256 and |UV| ≤ 1024, plane-quantization UV
  error is below 0.002137 texel per component. Adding reconstruction error is
  below 0.003114 texel, meeting the **1/256 texel** end-to-end budget.
- Depth/color plane drift is at most `560/2^16 = 0.008544921875` code units.
  Final rounded depth can differ by at most one D24 code from the ideal rounded
  plane. All 24 depth bits remain meaningful; the fixed-plane reference governs
  exact comparison decisions.

These bounds compare against ideal interpolation on the **snapped triangle**.
They do not bound geometry displacement introduced by clipping or Q4 snapping.
Conformance at discontinuous texel boundaries is defined in validation.md.

## Texture sampling

Texture coordinates are in texel-edge units: texel `(i,j)` occupies
`[i,i+1) × [j,j+1)` and its center is `(i+0.5,j+0.5)`. Nearest sampling chooses
`i=floor(u)`, `j=floor(v)` from the reconstructed coordinates. At an exact
integer boundary, this chooses the texel on the positive side.

For each axis independently, CLAMP maps the integer index into `[0,size-1]`;
REPEAT uses nonnegative mathematical modulo size. Dimensions are powers of two,
so repeat can use masking. Negative UV values use floor before addressing.
Fetch the little-endian RGB565 word at `base + pitch*j + 2*i`.

If color key is enabled and the raw sampled RGB565 word equals the key, discard
the fragment before either attachment is committed. Key comparison precedes
RGB expansion/modulation. A texture miss or provisional early depth test must
never make a keyed fragment write depth.

## Color, depth, and commit order

RGB888 colors are encoded channel numbers; there is no implicit sRGB/linear-light
conversion. For Gouraud, convert plane accumulators with
`C8=clamp(RNE(C_acc/65536),0,255)`. A flat color uses its exact supplied bytes.
Expand sampled RGB565 by bit replication:

```text
R8 = (R5<<3) | (R5>>2)
G8 = (G6<<2) | (G6>>4)
B8 = (B5<<3) | (B5>>2)
```

TEXTURE_REPLACE uses that expanded color. Modulation computes each channel as
`floor((texture8*color8 + 127)/255)`. The result is then packed as
`((R8>>3)<<11) | ((G8>>2)<<5) | (B8>>3)`; no dithering in this profile.
Replacement or modulation by white exactly preserves a source RGB565 texel.

Convert depth to `Z24=clamp(RNE(Z_acc/65536),0,0xFFFFFF)` or use a supplied
constant D24. Default comparison is LESS, clear is `0xFFFFFF`, near is zero.
LESS rejects equality and the exact clear/far value; LEQUAL accepts equality;
ALWAYS passes. Disabled depth testing passes independently of write enables.

For each covered sample, the observable order is: determine valid depth/color
inputs, compare depth, sample/shade if needed, apply key rejection, then commit
enabled color/depth writes as one logical fragment operation. A rejected fragment
writes nothing. A later fragment at the same pixel MUST observe the preceding
accepted depth update, even if its stages overlap in the pipeline. Forwarding,
interlocks, and early rejection are allowed if they preserve these semantics.

## 2D coverage

RECT covers integer half-open bounds intersected with viewport, scissor, and tile.
A pixel is a 1×1 RECT. Constant color/depth and common write/test flags apply.

LINE endpoints are integer pixel coordinates inside the viewport. CPU clipping
must occur before encoding. Sort endpoints lexicographically by `(x,y)` so
endpoint reversal is identical, then define the full closed pixel sequence:

```text
dx=abs(x1-x0); sx=1 if x0<x1 else -1
dy=-abs(y1-y0); sy=1 if y0<y1 else -1
err=dx+dy
loop:
    include pixel(x0,y0)
    if (x0,y0)==(x1,y1): stop
    e2=2*err
    if e2>=dy: err=err+dy; x0=x0+sx
    if e2<=dx: err=err+dx; y0=y0+sy
```

Both branch tests use the same e2. Apply scissor/tile intersection to this global
sequence; never restart from tile-clipped endpoints. An optimized line engine
may skip ahead if it returns exactly this set of samples. The degenerate line
includes its single endpoint once. Initial line color/depth are constant.

SPRITE covers integer destination bounds like RECT. For covered destination
pixel `(x,y)`, sample source texel
`(src_x+x-dst_x0, src_y+y-dst_y0)` at its center, with raw RGB565 replacement
and optional key rejection. Initial sprites have 1:1 size and no flip/rotation.
Clipping retains the same mapping across tiles. Depth defaults off for 2D
records but can be enabled with their constant D24 value.
