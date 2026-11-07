# Hardware compatibility policy

The user prohibits backward compatibility that consumes additional hardware
resources in any of their projects. Treat this as a hard design constraint in
this repository. Never retain or add legacy protocol decoders, compatibility
modes, fallback engines, bridges or duplicate datapaths solely to preserve old
interfaces when they cost LUTs, flip-flops, RAM, DSPs, pins or other hardware
resources. There is no small-cost exception.

Break the provisional ABI when needed and update the current host software,
tests and both authoritative GPU guides together. Compatibility may remain
only when it adds no hardware resource cost; establish that from implementation
and resource evidence rather than assuming a legacy branch is free.
Required current functionality and input validation are not compatibility-only logic; do not remove them
merely because they involve formats, versions or multiple data encodings.

# GPU documentation maintenance

The user requires a single obvious home for the GPU's current documentation:

- `GPU_DOCUMENTATION/GPU_ARCHITECTURE.md` explains the implemented GPU,
  responsibilities, rendering pipeline, storage, capabilities and limitations.
- `GPU_DOCUMENTATION/GPU_CONTROL.md` explains CPU control, logical bus behavior,
  packets, memory structures, synchronization/recovery and generic code examples.

Keep that directory dedicated to these **two authoritative guides**. Other
documentation under `docs/` provides detailed specifications, historical
measurements and supporting evidence; it must not silently override the guides.

Whenever the GPU architecture **or** CPU control changes, review and update
**both guides in the same change**, then commit and push them to GitHub with the
implementation. This is an explicit standing user requirement. It applies to
rendering behavior, numeric contracts, storage/resource allocation, scheduling,
clock/interface choices, wire formats, command semantics and recovery. Update
source references and examples as needed. Do not leave the guides stale after
an implementation change. Never claim that a local-only documentation update
has been pushed.

Distinguish implemented RTL from the software oracle, older C++ hardware models,
proposals and unresolved board integration. Label nominal clock/performance
assumptions and scoped resource measurements accurately. Do not invent physical
pin assignments, electrical timing, an RP2350 driver or hardware qualification.

Keep the generic control examples portable and serialize fields explicitly;
native C struct layout is not a wire format. Verify example changes with
`make gpu-docs-test`. After committing, run
`python3 scripts/check-gpu-docs.py --base origin/main --head HEAD` before pushing
(use `python` in MSYS2 UCRT64). CI checks that both guide contents accompany
protected implementation changes; passing that check does not replace reviewing
their technical accuracy.

Retain existing user changes, avoid force-pushing, and follow any later user
instructions about branches or publication. The standing documentation rule
does not authorize unrelated changes.

# Whole-board fit evidence

Hardware-fit and remaining-headroom claims for the Tang Nano 20K must use the
complete `tang20k_top` implementation, including PLL/dividers, reset crossings,
video serializers/pads, physical host wrapper, SDRAM boundary registers and the
actual package constraints. A logical-core serial probe excludes board logic
and includes test-only overhead; its LUT count is not production-board headroom.

Require source-matched synthesis, legal placement/routing, fixed SiP connection
checks and the selected board clock/capture timing gates before publishing a
programming image. Keep the image checksum and board report together. If RTL,
constraints or build inputs change, old implementation reports are historical
until rebuilt; do not qualify changed sources using an earlier route. Distinguish
tool-estimated internal timing from external electrical margins and physical
hardware measurements. Placement-seed trials are software build choices, not
additional hardware modes.

> **Addendum, 2026-11-07: temporary, deliberate exception to the rule above.**
> The user has decided to break this publishing rule for now, because stopping
> progress is not an option. The open-source flow (Yosys, nextpnr-himbaechel,
> Apicula) produced a GPU image that failed on the board: its host address
> latch did not work, although its RTL, synthesis and wiring were cleared
> separately. That flow is abandoned for board images. The repository image is
> now built with Gowin EDA V1.9.8.11 Education. That is the only copy available
> to the user: an unsigned installer from a third-party mirror, because Gowin's
> official download returned HTTP 403. Its synthesized netlist is encrypted.
> As a result:
>
> - The **fixed SiP connection check cannot be performed** from the netlist.
>   The SDRAM connection is evidenced only by hardware behaviour: font upload,
>   copies and scanout work on the board.
> - The **toolchain provenance is unverified**.
>
> The other gates still apply: source-matched synthesis, legal
> placement/routing, verified package pins, core/pixel/host timing, functional
> simulation, and the image checksum kept with its report. Every remaining
> timing violation must be listed in the report. This exception ends when an
> official, verifiable Gowin EDA can be obtained. The image must then be
> rebuilt and fully re-qualified.
