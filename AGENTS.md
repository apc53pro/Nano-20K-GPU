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
