# Full-GPU resource and fit proof

The complete GPU currently has **no exact mapped resource count or full-device
fit proof**. The native selections are 39 BSRAM blocks and 46 signed 18x18
multiplier equivalents. Full-GPU LUT, FF, clocking, I/O, packing and routing
requirements remain unqualified. The next hardware milestone is complete
functional RTL integration, followed by synthesis and placement/routing for
the Tang Nano 20K's exact `GW2AR-LV18QN88C8/I7` device.

`make resource-report` reports the evidence and missing implementation coverage.
`make fpga-fit` is the stricter acceptance gate: it must fail while complete,
current full-GPU evidence is missing. An incomplete result means the requested
certainty has not been obtained. A successful report generator, partial RTL
build, or capacity subtotal must never be presented as a full fit result.

The current [implementation manifest](../../rtl/full-gpu-manifest.json) lists
16 required blocks; only the arithmetic submodule has RTL. The checked-in
[resource-status snapshot](resource-status.json) records the missing coverage.
`make resource-report` regenerates `build/resource-report.json` from the built
system hardware profile and checks the recorded arithmetic source hashes.
It does not rerun synthesis or validate a complete netlist. The strict
`make fpga-fit` command currently exits with failure because that proof is absent.

## Evidence available now

| Evidence | Scope | What it establishes |
| --- | --- | --- |
| C++ functional reference | Rasterization and image/depth rules | An oracle for functional comparisons |
| Clocked system model | Selected bus, memory and rendering schedules | Cycle behavior under documented assumptions |
| Native primitive selection | 39 BSRAM and 46 multiplier equivalents | The declared bank/port and arithmetic budget |
| Register planning subtotal | 6,404 selected payload bits | Part of the intended storage mapping |
| Reciprocal/UV RTL and routed probe | Arithmetic core only | Mapped resources and timing for that one source/configuration |
| Complete functional GPU RTL | Missing | Required before whole-device resource counts exist |

The existing [UV qualification](rtl-qualification.md) reports 37 retained
multiplier equivalents, four BSRAMs, 4,472 FFs and 8,718 LUT4s for its arithmetic
probe. Its FF total includes 230 stimulus/checksum harness bits. The software
budget selects 40 multiplier equivalents for that arithmetic; synthesis removes
some constant partial products. This difference demonstrates why an instance
budget and a post-mapping count are different measurements.

The arithmetic result is not the remaining GPU's cost, and subtracting harness
FFs then adding software register subtotals does not produce a whole-GPU count.
The probe reached 89.17 MHz against a 100.8 MHz target. Routing succeeded for
that subdesign; its target timing did not pass. No full-GPU resource or clock
claim follows from it.

## Why the C++ model cannot supply exact LUT/FF totals

The model does not instantiate a physical register or LUT for every C++ field
or operation. Counting `sizeof()` includes host ABI padding, pointers,
containers and simulator state. Counting just declared numeric field widths
also misses optimization, sharing, FSM encoding and combinational logic.

Concrete examples:

- The 26 `PerspectivePipeline::Stage` aggregates are software views of live
  signals. Dead fields between operations become wires or disappear in RTL;
  they are not 26 physical copies of every aggregate field.
- `OpaqueTile` retains complete `Triangle` and plane objects. Some geometry is
  used only at command capture, and equivalent/dead copies may be eliminated.
  Simultaneously live lookahead and current-primitive data still need distinct
  physical storage or an explicitly verified sharing schedule.
- Tile-bank pointers select between two fixed bank sets. Their physical
  realization is a role selector and muxes, not host-width pointer registers.
- SDRAM absolute 64-bit deadlines and host-link cycle arithmetic express
  timing behavior. Bounded physical counters, phase state and comparisons must
  be defined in RTL and verified against those schedules.
- Staged native-port objects may represent combinational wires rather than
  clocked storage. Native DSP registers must not be counted again as general FFs.
- Adders, variable shifts, multiplexers, comparisons, decoders, reset and clock
  control consume logic even when their C++ result occupies no persistent field.

The selected 6,404-bit payload subtotal is consequently neither an exact FF
count nor a lower bound. The eight optimization statistics are report-only
host counters today. A future implementation of all eight as live hardware
profiling counters would require a 512-bit reserve plus readout/control logic;
that optional reserve is not proof of existing physical FF use.

## State classification required at the RTL boundary

Every retained data or control item needs an owner, lifetime, width/depth,
clock/reset, port schedule and implementation class:

| Class | Examples | Required evidence |
| --- | --- | --- |
| Native BSRAM/ROM | Tile banks, FIFOs, context banks, seed tables | Primitive configuration, address/width bounds and legal port access |
| Native DSP | Setup, perspective, address and shading products | Primitive modes, register settings and mapped cells |
| General registers | Queue state, descriptors, working planes and handoffs | Explicit RTL widths, live ranges, reset/enable behavior and mapped FFs |
| Combinational logic | Coverage, depth comparisons, addressing and decoding | Synthesized/mapped logic and timing paths |
| Constants and build configuration | Capabilities and selected timing parameters | Constant specialization and ROM/logic mapping |
| Host-only model state | Oracles, observations and simulator bookkeeping | Explicit exclusion from device execution and resource totals |

Host-only state includes framebuffer/display snapshots, diagnostic payload
mirrors, trace identities and scoreboards, CSV/JSON data, CPU scene preparation,
`HostPassCompiler`, and the SDRAM model's external-memory/defined-byte arrays.
These arrays do not create on-chip RAM or extra physical memory ports.

An existing boundary issue needs an explicit physical contract:
`SystemModel::status()` exposes `uninitialized_read_words`, but that value
depends on the SDRAM simulator's host-only defined/poison shadow. A physical
design cannot obtain that diagnostic signal for free. Either implement and
budget an actual tracking mechanism, or define its unsupported/reserved
semantics through a capability/versioned status contract. Apply the same rule
to every status field or control decision derived from simulator observations.

## Full-fit acceptance criteria

A full resource/fit result requires all of the following:

1. **Complete functional RTL.** Integrate coverage/interpolation/depth/2D,
   perspective arithmetic, texture/cache, shading and fragment contexts, tile
   transfers, record/bin decoding, host link, shared SDRAM controller, scanout,
   reset/recovery, and required status/capability paths. A shell containing RAM,
   DSPs, register arrays, dummy traffic or checksums is not this implementation.
   Standard video-output IP may be adopted without developing a new HDMI
   protocol, but its logic, serializer, clock and I/O footprint must be included.
2. **Defined device interfaces.** Resolve clock ownership/domains, CDC and reset
   crossings, package pins, bank voltages, SDRAM interface timing and external
   host setup/hold constraints. The model's common derived-clock assumptions
   are not proof of a later independently clocked interface.
3. **Observable and verified behavior.** Real command inputs must cause the
   specified rendering and display outputs. Run functional and cycle comparisons
   against the SRR, including stalls, buffer ownership, malformed commands,
   partial packets, refresh, misses, reset and abort. Confirm all required
   functions survive synthesis rather than being optimized out as unused logic.
4. **Exact-part synthesis and mapping.** Use the selected build configuration
   and exact device/package, with no unaccounted black boxes or behavioral
   fallbacks. Record mapped LUT4, FF, BSRAM, DSP, PLL, I/O and other applicable
   primitive counts. Check native packing restrictions and hard-block modes,
   not only aggregate 18x18 equivalence or nominal bit capacity.
5. **Successful complete-top placement and routing.** Count the integrated
   device's retained cells and verify all routes and hard-block allocations.
   A submodule report, out-of-context result or sum of module estimates cannot
   replace this result. Aggregate counts within capacity are necessary but
   do not prove routability.
6. **Complete timing constraints and closure.** Check every required clock and
   constrained I/O path. Define legitimate asynchronous groups and exceptions
   explicitly; unexplained unconstrained paths must prevent a timing claim.
   A routed design that misses its required clocks has physical resource
   evidence but has not qualified the planned operating point.
7. **Fresh, reproducible provenance.** Record tool/database versions, device,
   top, configuration, source/ROM/constraint hashes, commands, exit status and
   generated reports. Reject stale or mismatched artifacts. State the scope of
   every result; missing evidence remains unknown rather than becoming zero.

The final counts are exact for that mapped/routed source, configuration and
tool run. Later RTL, constraints or feature changes require new qualification.
Board bring-up still validates electrical behavior, memory operation and
display/host integration; place-and-route does not replace those measurements.

## Required implementation sequence

The following are work packages for the next RTL milestone, not completed RTL:

1. Freeze the selected hardware configuration, clock/reset/CDC boundaries,
   physical interfaces and supported diagnostic semantics. Establish the complete
   top and a manifest that leaves missing blocks explicitly incomplete.
2. Implement shared SDRAM service, initialization/refresh/arbitration and
   scanout as a verified memory/display slice using real native ports.
3. Implement host packet transport, bounded queues, command execution, resource
   ownership, fences, readback and recovery against the SRR's byte stream.
4. Implement resident tile storage, native transfer paths and coverage/depth/2D
   rendering; compare memory contents and per-cycle acceptance/retirement.
5. Integrate the existing UV RTL with actual texture/cache, ordered fragment
   context memories and shading. Recheck native resources and full-system
   behavior after each functional slice.
6. Integrate the full selected top, include adopted video/clocking IP, then run
   exact-part synthesis, placement/routing and timing. Optimize the measured
   full-design bottlenecks and rerun all affected comparisons.

The report/gate work makes the missing proof visible and prevents accidental
fit claims. It does not complete these implementation packages or establish
that the complete GPU fits. The decisive next milestone is the functional
full-GPU RTL and its exact-device mapped/routed report.
