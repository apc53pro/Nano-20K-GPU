# Lossless upload decoder measurement

This is historical evidence from commit `7705fee`, before the unified 0.2
upload implementation. For the current protocol and integration status, read
the [architecture](../../GPU_DOCUMENTATION/GPU_ARCHITECTURE.md) and
[control](../../GPU_DOCUMENTATION/GPU_CONTROL.md) guides. References below to
unchanged production RTL describe the state when this experiment was measured.

This experiment measures a concrete first upload-compression candidate before
changing the live GPU. **Production RTL, N20K-SYS-0.1 packets, 320-byte SDRAM
records, firmware and the frozen clock implementation are unchanged.**

The results support a small, fast standalone decoder. They do **not** establish
that the complete GPU can accommodate the integrated upload path: the full
logical GPU plus independently stimulated decoder failed legal placement in
the tested co-residency probe. There is no routed frequency for that failed
probe. Its extra measurement registers and lack of actual upload integration
limit what that failure establishes.

## Measured candidate

Each existing 320-byte record is treated as eighty exact little-endian u32
words. Twenty groups each contain one descriptor byte followed by payloads
for four words. Each two-bit descriptor selects:

| Code | Payload | Reconstructed word |
| --- | --- | --- |
| 0 | None | Zero |
| 1 | One byte | Sign-extend signed 8-bit to 32 bits |
| 2 | Two bytes | Sign-extend signed 16-bit to 32 bits |
| 3 | Four bytes | Original 32 bits |

Both halves of every 64-bit coefficient are preserved independently. No
fixed-point scale, coefficient, coverage rule, state field or rounding rule
changes. Unlike a mode-specific format, this also retains arbitrary nonzero
bits in fields normally unused by a rendering mode. The descriptor register
is only eight bits; the decoder has no record buffer, BSRAM or DSP allocation.

The encoded length is 20..340 bytes. A future uploader could use existing raw
uploads when compression grows a record, but that selection and a compressed
packet opcode are **not implemented**. These results do not measure the
previously discussed shared-state lookup or compact resident-record designs.

## Standalone exact-device synthesis and routing

Target: **GW2AR-LV18QN88C8/I7**, GW2A-18C database, LUT4 mapping
(`synth_gowin -nowidelut`), nextpnr seed 1. Both standalone variants use the
same serial input/output probe with 56 measurement register bits.

| Resource, including identical probes | Raw byte assembler | Decoder | Difference |
| --- | ---: | ---: | ---: |
| Routed physical LUT sites | 123 | 175 | +52 |
| Flip-flops | 100 | 109 | +9 |
| BSRAM | 0 | 0 | 0 |
| MULT18X18 equivalents | 0 | 0 | 0 |
| Estimated Fmax | 274.57 MHz | 263.23 MHz | — |

The bare decoder maps to **82 LUT1–LUT4 cells, 10 ALU cells and 53 DFFs**.
Mapped cells and routed physical LUT sites are different inventories: ALU
placement and FF feed-through LUTs affect the latter. The +52/+9 probe
difference is not an established resource increment in the live GPU.

Both isolated probes pass their 68.88 MHz timing constraint. These are static
timing estimates from modeled cell/wire delays, not real-board measurements.
The decoder's isolated Fmax does not imply that the GPU can run at that clock.

Full evidence: [standalone resources/timing](upload-codec-measurement.json).

## Full logical GPU co-residency check

The [co-residency probe](../../experiments/upload-codec/gpu_codec_probe.sv)
retains the complete existing GPU and all decoder inputs/outputs. It adds
11 stimulus and 44 capture bits to the original probe. The decoder receives
independent stimulus and does not replace or intercept the GPU upload path.

The tested configuration synthesized and packed, but the placer could not
find a legal placement. Packing's optimistic physical-LUT lower bound was
**20,222 of 20,736**, which is not proof of legal placement. The frozen GPU
baseline remains **20,305 routed physical LUT sites, 46 BSRAM and 25 DSP
equivalents**. Do not add the isolated +52 figure to the baseline and treat
that sum as a fit result.

This one failed placement does not prove that an integrated decoder is
impossible. It prevents a positive fit claim for this measured candidate.
Actual integration would change packet validation, staging and data routing,
and could reuse existing registers or replace some raw assembly logic.
Those changes have not been measured or promoted to production.

Full evidence: [combined probe result](upload-codec-gpu-measurement.json).

## Exact reconstruction and cycle measurements

Tests passed for **1,969 records / 157,520 output words / 157,522 checks**:
1,920 compiled scene records plus synthetic zero, sign-extension boundaries,
raw/random words, independent input/output stalls and reset cancellation.
All eighty reconstructed words match the original record before completion.

For an always-valid source and always-ready output, actual RTL simulation
measures **encoded bytes + 80 core cycles**, excluding one record-start cycle.
Output can produce a word per cycle for zero runs. This is record expansion,
not fragment or pixel rendering throughput.

Frame-zero triangle results at scene time 1.25 s:

| Scene | Triangles | Original bytes | Encoded bytes | Mean encoded bytes/triangle | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| Showcase | 18 | 5,760 | 2,562 | 142.3 | 55.5% |
| Perspective | 8 | 2,560 | 1,015 | 126.9 | 60.4% |
| Overdraw | 28 | 8,960 | 4,639 | 165.7 | 48.2% |
| Opaque | 69 | 22,080 | 5,799 | 84.0 | 73.7% |

No sampled scene record needed raw fallback. This is scene-dependent, not a
guaranteed compression ratio. Packet/command framing and addresses are excluded.
All primitive types were checked; frame-zero record payloads including sprites,
rectangles and lines fall from 112,640 to 20,233 bytes in showcase.

Separate RTL runs use a synthetic 25.2 MHz offered-byte source against an
**assumed** 68.88 MHz core (phase increment 15/modulus 41), with a one-byte
pending slot and an always-ready sink or a sink accepting once per four
cycles. Source phase restarts per record; missed source slots do not accumulate.
Every run checks all 1,920 records. The JSON reports per-scene cycles and
assumption-derived microseconds. It does not model the packet parser, clock
crossing FIFO, staging RAM, SDRAM writes, CPU preparation or frame rendering.

The expander preserves the existing 320-byte SDRAM footprint and repeated
per-tile record fetch traffic. Texture uploads and tile references are also
unchanged. **No integrated upload speed or frame FPS improvement is measured.**

Full evidence: [correctness, payloads and standalone cycles](upload-codec-throughput.json).

## Reproduction

Use the existing C++17 and RTL-tool setup. On Linux:

```sh
python3 scripts/check-upload-codec.py --report docs/hardware/upload-codec-throughput.json
python3 scripts/measure-upload-codec.py --report docs/hardware/upload-codec-measurement.json
python3 scripts/measure-upload-codec-gpu.py --report docs/hardware/upload-codec-gpu-measurement.json
```

Use `python` instead of `python3` in MSYS2 UCRT64. The final command returns
nonzero when placement fails; retain that outcome rather than describing it
as successful routing. Verbose logs, source snapshots, vectors and netlists
remain under ignored `build/upload-codec-measurement/`. Versioned JSON records
source hashes and tool versions; the failed combined run has no routed result.
