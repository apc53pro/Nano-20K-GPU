# Shared reciprocal and exact UV reconstruction

The streaming arithmetic block in
[reciprocal.hpp](../../reference/include/nano_gpu/hw/reciprocal.hpp) and
[hw_reciprocal.cpp](../../reference/src/hw_reciprocal.cpp) accepts one `{U,V,q}`
sample per enabled core edge. Both coordinates share one reciprocal estimate.
The fixed schedule produces a registered result after **26 enabled edges**,
counting the operand-capture edge. A parent captures that established result on
a later common edge. A shared clock enable freezes every stage and output.

The result is the exact canonical expression
`RNE(attribute * 2^26 / q)`, including signed Q16 endpoint behavior. Approximate
arithmetic supplies a quotient estimate; bounded exact remainder correction
removes its error. This avoids introducing texture-boundary differences while
comfortably meeting M0's 1/1024-texel reconstruction requirement.

## Resource selection

All wide products use 17-bit positive limbs in signed native 18x18 multipliers.
The seed residual and Newton error are signed native operands. Every multiplier
uses the selected three-enabled-edge registered Gowin configuration; the
consumer captures its old result on a fourth edge.

| Operation | Native multiplier equivalents |
| --- | ---: |
| Linear seed residual18 x gradient16 | 1 |
| Normalized divisor33 x seed35 | 6 |
| Seed35 x Newton error18 | 3 |
| Two attribute magnitudes43 x reciprocal35 | 18 |
| Two exact divisors38 x quotient estimates32 | 12 |
| **Total** | **40** |

Four SDPB 1024x16 instances provide the fixed 256-entry seed ROM: three banks
for the 35-bit base and one for its 16-bit gradient. Configuration INIT bits
supply the constants; no startup write loop or runtime division generates
them. Their synchronous outputs cross a separate capture edge.

The checked-in images and C++ constants come from
[generate-reciprocal-rom.py](../../scripts/generate-reciprocal-rom.py).
`python3 scripts/generate-reciprocal-rom.py --check` detects drift between the
software and RTL inputs. The ROM uses fewer words than each physical bank
provides; spare words do not create additional independent ports.

These counts select arithmetic and memories. Additional delayed operands,
signs, tags, valid/error bits, adders and correction logic still consume
registers and LUTs. Actual synthesis and timing reports determine their fit;
the count of40 DSP equivalents alone does not establish whole-system fit.

## Integer algorithm

For valid `2^29 <= q <= 2^37`, choose `e=floor(log2(q))` and normalize to the
unsigned Q32 value `D=RNE(q * 2^(32-e))`. If rounding gives `D=2^33`, replace it
by `2^32` and increment `e`. This carry case matters for divisors immediately
below a power of two.

The normalized real divisor `d=D/2^32` lies in `[1,2)`. Its high eight fractional
bits select segment `i` with center `t=1+(2*i+1)/512`. The ROM stores:

```text
B[i] = RNE(2^34 / t)                # 35-bit Q34 reciprocal base
G[i] = RNE(2^16 / t^2)              # 16-bit unsigned gradient magnitude
delta = RNE((D - t*2^32) / 2^7)    # signed Q25; fits signed18
R0 = B[i] - RNE(delta*G[i] / 2^7)  # Q34 linear reciprocal seed
E = 2^34 - RNE(D*R0 / 2^32)        # signed18 Q34 Newton residual
R = R0 + RNE(R0*E / 2^34)          # corrected Q34 reciprocal
Q = RNE(abs(attribute)*R / 2^(e+8))
```

`RNE` always rounds exact ties to an even integer, including negative products.
The model does not use host floating point, division or a general-purpose wide
multiply in these stages. Explicit 96-bit add/shift wiring combines native
partial products.

Compute the exact residual `r=(abs(attribute)<<26)-q*Q`. Two registered correction
stages each perform one of:

```text
if r < 0:  Q -= 1; r += q
if r >= q: Q += 1; r -= q
otherwise: retain Q,r
```

The corrected pair satisfies `0 <= r < q`. The final stage rounds upward when
`2*r > q`, or when equality holds and `Q` is odd, then restores the input sign.
Positive results above `0x7fffffff` and negative magnitudes above `0x80000000`
report overflow instead of wrapping.

An early magnitude bound `(q<<5)+1024` admits every representable signed Q16
result, including negative-endpoint ties. Larger input magnitudes carry an
overflow token while safe zero data passes through the arithmetic. Invalid q
similarly carries an error token. Both retire in order at the normal latency.

## Error bound and exact correction

For a segment, `|d-t| <= 2^-9`. The relative error of the exact tangent-line
reciprocal is `(d-t)^2/t^2 <= 2^-18`. Quantized delta, gradient and final seed
rounding add less than `2^-24 + 2^-33 + 2^-42` to that relative bound. Therefore
the initial relative error has magnitude below `3.876e-6`, and the Q34 Newton
residual stays within signed18.

One Newton step squares that error. Rounding its residual and reciprocal update
adds at most half a Q34 unit each. The resulting absolute reciprocal error is
below `7.4e-11`. Normalizing the original divisor contributes at most `2^-33`
absolute error in `d`. Together these give relative quotient error below
`2^-31`, throughout the valid divisor range.

The retained magnitude bound makes the exact unrounded quotient at most
`2^31+128`. Reciprocal error plus quotient rounding therefore differs from the
exact real quotient by less than1.501 Q16 integer codes. Its distance from the
exact floor quotient is at most two codes. Two signed residual corrections
establish the exact floor, and the final remainder-based RNE establishes the
exact canonical signed result. The argument applies to the full signed Q16
result range, not only the usual M0 vertex domain of +/-1024 texels.

Independent rational tests include divisor endpoints and normalization carries,
seed-segment boundaries, signed ties, Q16 extrema, invalid divisors, overflowing
64-bit attributes, continuous traffic, bubbles, clock stalls and reset.

## Enabled-edge schedule

Stage numbers below start at zero for the capture edge.

| Stage | Operation |
| --- | --- |
| 0 | Capture divisor/signs and43-bit magnitudes; reject high input bits |
| 1 | Divisor-dependent magnitude bound, normalization and seed index |
| 2 | Native ROM read and residual rounding from OLD stage1 normalization |
| 3 | Later base capture and OLD ROM-gradient/residual fanout to seed DSP |
| 3 / 4 / 5 / 6 | Seed multiplier input / product / output / parent seed capture |
| 7 / 8 / 9 / 10 | D x R0 input / product / output / Newton error capture |
| 11 / 12 / 13 / 14 | R0 x E input / product / output / corrected reciprocal |
| 15 / 16 / 17 / 18 | Both UV products input / product / output / quotient estimates |
| 19 / 20 / 21 / 22 | Exact q x Q products input / product / output / residuals |
| 23 / 24 | First / second exact floor correction |
| 25 | Exact RNE, sign restoration, overflow flags and registered output |

The6-bit context tag crosses the same26 enabled edges. Pipeline bubbles retain
their invalidity; a valid result is never inferred from elapsed host time.
