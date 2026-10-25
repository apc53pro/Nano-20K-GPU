# Conservative triangle binning review prototype

Baseline implementation: `b434df20a15411f17cea24ad860fa413d5c38af1`.
This is a review-only experiment, not the production binning implementation.
The only source change is the single hunk in `prototype.patch`, applied to a copied `renderer.cpp`; source files in the repository are not edited.

## Reproduce

From the repository root, with the ordinary C++17 compiler, Make, and Python build prerequisites installed:

```bash
python3 docs/benchmarks/fine-binning-review/reproduce.py --repo . --output-dir build/fine-binning-review
```

In MSYS2 UCRT64 use `python` if that is the configured interpreter. The script selects the `.exe` suffix on Windows.
It builds ordinary headless objects, compiles the copied/modified renderer, links that object in place of `renderer.o`, and runs the existing four-scene cold/steady benchmark with `--verify`.
Use `--no-build` to reuse existing objects, `--build-dir` for an alternative object directory, or `--cxx` for a single compiler executable.
The patch context must match exactly; the script refuses to guess if the baseline source has changed.

## Experiment

At each candidate triangle/tile intersection, evaluate the maximum of each unbiased edge equation over the clipped rectangle of pixel centers. If any maximum is negative, or zero for an exclusive edge, the triangle cannot cover a pixel there and its bin reference is omitted.
All edge tests use signed 64-bit CPU intermediates. The original triangle bounds, immutable plane anchor, coefficients, list ordering and GPU record format remain unchanged. No per-pixel coverage work is moved to the CPU.

At time zero, the prototype eliminated 305/633 triangle references in showcase, 238/413 in perspective, 989/2401 in overdraw, and 2482/3539 in opaque. A separate exhaustive check found zero covered samples in rejected tiles for these scenes.

## Validation and results

Each scene ran once cold and twice frozen (`--time 0 --step 0 --frames 2`). Every run used `--verify`, which compares RGB565 and D24 against tiled, reverse and monolithic canonical traversals. The benchmark additionally required identical committed-baseline hashes, zero scanout underflows and zero legacy/untimed SDRAM adapters.

| Scene | Steady pass (ms) | Steady upload/render submission (ms) | Color hash | Depth hash |
| --- | ---: | ---: | --- | --- |
| showcase | 25.132 | 30.035 | `1d85d76382a08afb` | `cdb41a22de37f530` |
| perspective | 20.302 | 24.259 | `8a3238862a3b0b9e` | `86c2615dc2b84f7e` |
| overdraw | 31.884 | 35.692 | `98b7847230519f8` | `178db08d99fd56f7` |
| opaque | 20.350 | 21.543 | `af59c4be63544c6b` | `42879846195fd030` |

These are measured model cycles at the assumed 100.8 MHz core/SDRAM and 25.2 MHz host clocks, not wall-clock simulator rates or physical FPGA results. Steady submissions exclude boot, RP2350 CPU preparation and PRESENT/scanout-observation waits.
The prototype requires no additional FPGA BSRAM, DSPs or memory ports. RP2350 execution cost for the additional CPU bin tests has not been measured.

## Contract and regression work before adoption

`docs/spec/architecture.md` currently requires a reference for every bounding-box tile. That wording must be clarified to permit omission of provably empty tiles. Reference/candidate counters and tests must follow the refined binning contract.
Production validation should cover inclusive/exclusive shared edges, all subpixel positions, clipped/scissored windows, thin/pixel-free intersections and unchanged immutable interpolation anchors. This experiment verified the four frozen demo scenes; it is not a substitute for the full production regression suite.
