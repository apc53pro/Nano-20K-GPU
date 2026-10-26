# Reciprocal/UV RTL qualification

For the integrated host-to-scanout RTL implementation and its Verilator
simulator, see [Integrated RTL GPU](rtl-gpu.md). The results below qualify the
separate reciprocal/UV arithmetic probe, including its own wrapper and routing
constraints; they do not qualify the integrated GPU's fit or frequency.

The prototype in [uv_pipeline.v](../../rtl/uv_pipeline.v) implements the same
exact corrected reciprocal algorithm and 26-enabled-edge interface as
[hw_reciprocal.cpp](../../reference/src/hw_reciprocal.cpp). It accepts one U/V/q
sample per enabled edge and holds every stage when output backpressure or CE
prevents advancement. Reset cancels all retained valid tokens.

This is an arithmetic-core qualification, not a complete GPU RTL implementation.
The texture/cache, fragment context banks, shading, tile DMA, host link and
scanout are outside this RTL top. The selected complete GPU's resource budget
and software-model cadence do not establish whole-device fit or clock timing.

## Reproduce the checks

On Linux x86_64, install the pinned tools into the ignored workspace directory:

```sh
./scripts/setup-rtl-tools.sh
./scripts/setup-rtl-tools.sh --check
python3 scripts/rtl-check.py
python3 scripts/rtl-check.py --synth
python3 scripts/rtl-check.py --pnr
```

The automatic installer downloads OSS CAD Suite release **2026-10-24**, checks
the archive's pinned SHA256, and extracts it under `.deps/rtl`. It changes no
system installation. The approximately 749 MB download is not needed to build
or run the SRR. `--check` reuses an existing installation without downloading.

Windows users can install OSS CAD Suite for their platform and put its `bin`
directory on PATH, or set `NANO_GPU_RTL_TOOLS` to its installation directory.
Use `python scripts/rtl-check.py` in MSYS2 UCRT64. Automatic bootstrap currently
supports Linux x86_64 only; the renderer's normal Windows setup is unchanged.
`PYTHON=python ./scripts/setup-rtl-tools.sh --check` selects that interpreter.

Reports and tool logs go to `build/rtl` by default; `--build-dir PATH` selects a
different directory. `--pnr` includes simulation and synthesis. Add
`--require-timing` to fail the command unless the actual routed clock meets the
100.8 MHz target. Without that option, successful routing can still report
`target_met=false`; reading that field is required before making a clock claim.

## Independent functional and cycle checks

The vector generator uses the independent integer rational oracle in
[uv_oracle.hpp](../../reference/tests/uv_oracle.hpp), rather than computing
expected values with the implementation's reciprocal arithmetic. The RTL test
covers 68,237 vectors: legal M0 coordinates, the full signed-Q16 result domain,
normalization and seed boundaries, signed ties, overflows and invalid divisors.

The testbench checks the 26-enabled-edge latency and a continuous one-sample
accept/retire cadence, then injects CE stalls and independent output
backpressure. It requires held outputs to remain stable and every accepted
sample to retire with the original context tag. A final reset sequence checks
cancellation and reset priority over CE.

Simulation uses the selected three-edge multiplier and synchronous ROM behavior.
Synthesis instantiates actual Gowin `MULT18X18` and `SDPB` primitives with the
documented register and INIT settings. This is not vendor-library or board-level
co-simulation; the source parameters and primitive timing assumptions remain
part of the implementation contract.

## Physical scope and recorded evidence

The final routed qualification reached **89.17 MHz**, with a **100.8 MHz**
constraint: **the target was not met**. The arithmetic probe uses 37 retained
native multiplier equivalents, four BSRAMs, 4,472 FFs and 8,718 LUT4 resources in
nextpnr's utilization report. The synthesis-selected 40-multiplier model remains
a conservative allocation; constant partial products are optimized away.

The remaining 11.214 ns critical path runs from the stage17 exponent register
through variable quotient extraction and RNE into the stage18 estimate. It has
6.045 ns of reported logic delay and 4.903 ns of routing delay, plus clock/setup
terms. Further timing work must address that measured path. The unit's
26-cycle/II1 functional result does not justify treating 100.8 MHz as an achieved
FPGA frequency.

The placed-and-routed target is the Tang Nano 20K's exact
**GW2AR-LV18QN88C8/I7**, with the **GW2A-18C** nextpnr database. The tools do not
substitute another package. [uv_probe.v](../../rtl/uv_probe.v) wraps the core in
a 198-bit serial stimulus register and a 32-bit observable checksum, keeping the
whole datapath live while requiring only nine package pins. The probe includes
230 bits of harness state; its total FF count is not the standalone core count.
The [CST](../../rtl/uv_probe.cst) belongs to this qualification probe and is not
the GPU board's pin assignment.

The C++ resource selection counts 40 native multiplier equivalents and four
seed-ROM banks. Synthesis can remove constant partial products, so the actual
retained multiplier count may be smaller. Neither count grants extra ports or
proves the remaining GPU logic fits.

The checked-in [qualification summary](rtl-qualification.json) records the
actual tool versions, device, retained resources, routed clock, target result
and input SHA256 hashes. The complete generated report additionally contains
nextpnr's detailed timing and utilization data. The script rejects a run if its
source inputs change while tools are running, preventing a stale report from
being attributed to a later implementation.

Use a fresh `--build-dir` for each qualification run. A failed rerun can leave
older files in an existing directory; an old `qualification.json` is not proof
that the failed command passed. Check the command exit status and compare the
recorded source hashes with the working tree before attributing a report to it.

These checks create simulation, synthesis and routing artifacts only. They do
not create or program a board bitstream. Hardware operating frequency remains
limited by the complete GPU's eventual timing closure and board validation.
