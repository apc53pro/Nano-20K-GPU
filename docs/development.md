# SDL2 development setup

The [software reference renderer](software-reference.md) uses SDL2 to display its
RGB565 framebuffer. Install the development headers/library and verify the
display path:

```bash
./scripts/setup-sdl2.sh
```

The installer reuses a working SDK. On Debian/Ubuntu with root or sudo, it installs
`build-essential`, `pkg-config`, and `libsdl2-dev`. On macOS it uses an existing
Homebrew installation for `pkg-config` and `sdl2`; Xcode Command Line Tools must
already provide a C++ compiler. On Windows it uses MSYS2's package manager to
install a matching native compiler, SDL2, pkg-config, Python, and make. See the
[Windows instructions](#windows-with-msys2) below; Debian/Ubuntu under WSL is an
alternative.

The renderer also needs make and a C++17 GCC/Clang compiler with `__int128`;
conformance tests need Python 3. On Debian/Ubuntu, install `python3` if missing.
On macOS, install Python 3 with Homebrew if it is not already available.

```bash
make -j4
make test
./build/nano-gpu-ref
```

To build and test without installing SDL2 or opening a window:

```bash
make -j4 headless test
./build/nano-gpu-ref-headless --headless --verify --output build/frame.ppm
```

Architecture throughput tests and benchmarks also run without SDL:

```bash
make -j4 throughput
make throughput-test
```

See [fragment throughput and clock traces](software-reference.md#fragment-throughput-and-clock-traces)
for per-cycle events, controlled benchmark measurements, and system-mode CSV
capture. System JSON reports modeled throughput independently of PC execution
time. The trace files and benchmark outputs belong under the ignored `build/`
directory and should be regenerated on each machine.

## Windows with MSYS2

Install [MSYS2](https://www.msys2.org/) and open **MSYS2 UCRT64** from the Start
menu. Check the installation before running setup:

```bash
cygpath -w /
command -v pacman
```

The root normally resolves to `C:\msys64` (or your chosen MSYS2 directory), and
`pacman` should be available. Git Bash is a separate environment: its root is
typically `C:\Program Files\Git`, and it does not include pacman. Git Bash can
also display `UCRT64`, so the prompt alone does not identify the installation.
Installing Bash alone does not provide the compiler or MSYS2 packages used here.
The recommended toolchain is 64-bit GCC under UCRT64. MINGW64 and CLANG64 shells
are also recognized by the installer, which selects packages for that shell.
Visual Studio/MSVC is not supported because the setup code requires `__int128`.

Update MSYS2 before installing dependencies:

```bash
pacman -Syu
```

If the update asks you to close the terminal, do so, reopen **MSYS2 UCRT64**, and
run `pacman -Syu` again. Then change to your checkout, for example
`cd /c/w/Nano-20K-GPU`, and run:

```bash
./scripts/setup-sdl2.sh
make clean
make -j4
make test
./build/nano-gpu-ref.exe
```

Keep your existing checkout; for example, `C:\w\Nano-20K-GPU` is accessible as
`/c/w/Nano-20K-GPU` from either environment. The clean rebuild discards objects
from other compilers such as TDM-GCC. After setup, `command -v g++` in UCRT64
should resolve to `/ucrt64/bin/g++`, rather than a Windows compiler elsewhere on
PATH. If you also want to run Git inside MSYS2, install it with
`pacman -S --needed git`.

The equivalent manual dependency installation for **UCRT64** is:

```bash
pacman -S --needed make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-python
```

The Makefile uses MSYS2's `python` command on Windows. This avoids the Windows
`python3` Microsoft Store execution alias shown when Python is missing. Run the
build and executable in the same MSYS2 shell so the selected toolchain's `bin`
directory supplies SDL2 and compiler runtime DLLs. Launching the executable from
Explorer or a different terminal may require those DLLs to be packaged separately.

If all build dependencies are already available, setup reuses them without
requiring pacman. `--check` only verifies the SDL SDK and never invokes a package
manager. When installation is needed but pacman is missing, setup reports the
current shell root to distinguish installation problems from PATH problems.

For a headless run in UCRT64:

```bash
make -j4 headless test
./build/nano-gpu-ref-headless.exe --headless --verify --output build/frame.ppm
```

Rebuild on each machine. Do not copy Linux/macOS build products or `.deps/sdl2`
into a Windows checkout. If changing compilers in an existing checkout, run
`make clean` before rebuilding or use a separate `BUILD_DIR`.

## Linux without administrator access

The default automatically selects a local build when root/sudo is unavailable.
To select it explicitly or choose a different installation directory:

```bash
./scripts/setup-sdl2.sh --local
# Optional: ./scripts/setup-sdl2.sh --prefix /absolute/path/to/sdl2-sdk
source .deps/sdl2/env.sh
```

Local builds need a working C/C++ compiler, make, pkg-config, curl, tar, and
sha256sum or shasum. For a fresh Debian/Ubuntu machine an administrator can install
these prerequisites with:

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config curl ca-certificates
```

This path downloads official SDL **2.32.4**, verifies a pinned SHA-256, and builds
a shared library and development files in `.deps/sdl2`. It follows SDL's
[documented configure/make installation](https://wiki.libsdl.org/SDL2/Installation)
with a local prefix. The selected [release](https://github.com/libsdl-org/SDL/releases/tag/release-2.32.4)
is pinned for repeatability; the script does not track the newest release.
The system package path uses the distribution's supported SDL2 version instead.

No system files or shell startup files are modified by local setup. Installed
libraries, cached downloads, activation settings, and logs are excluded from Git
under `.deps/`. The build log is `.deps/sdl2/setup.log`. Prefixes containing
whitespace are unsupported; select a different `--prefix` in that case.
Set `JOBS` to change the default four parallel build jobs, and `CC`/`CXX` to name
compiler executables. CMake is not required by this setup/check; a local SDK also
exports `CMAKE_PREFIX_PATH` for future CMake consumers.

Source the generated `env.sh` in each new build shell. It configures PATH,
pkg-config discovery, local shared-library lookup, and CMake prefix discovery.
For a custom prefix, source that prefix's `env.sh`. Installed files use absolute
paths: after moving the SDK/checkout, run `./scripts/setup-sdl2.sh --rebuild`
(with the same `--prefix` option if custom) to regenerate them. This also repairs
an interrupted local install. Recreating the SDK is preferable to copying its
binaries between machines.

## Verification

```bash
./scripts/setup-sdl2.sh --check
# Or run the dependency check directly:
./scripts/check-sdl2.sh
```

Verification compiles [sdl2-smoke.cpp](../tools/sdl2-smoke.cpp) as C++17, links
SDL2, initializes the dummy video backend and software renderer, uploads a
640×480 RGB565 streaming texture with 1,280-byte row pitch, and checks all pixels
in a red/green/blue readback in RGB565 format before presentation. Comparing in
RGB565 avoids relying on backend-specific RGB888 expansion rounding. Temporary
executable output is removed afterward. These checks need no desktop or graphics
acceleration.

The default local SDK is discovered automatically by the setup/check scripts.
The Makefile also discovers the default local SDK automatically. For a custom
prefix, source its `env.sh` or set `SDL2_PREFIX` when invoking make.
A visible SDL window needs a working desktop/video backend; local source builds
enable backends according to
the development headers available on that machine. A headless build/check does
not establish desktop-window compatibility. Prefer distribution development
packages for a desktop machine with full video backend dependencies.

The smoke test validates dependencies and RGB565 presentation. Run `make test`
for renderer conformance and `make sanitize` for address/undefined-behavior
sanitizer checks. In environments that prevent LeakSanitizer from running under
tracing, use `ASAN_OPTIONS=detect_leaks=0 make sanitize`; this disables only leak
checking. See the [renderer guide](software-reference.md) for the test coverage
and remaining model limitations.

## Integrated RTL simulator

The [RTL GPU](hardware/rtl-gpu.md) is a separate Verilator build. Its device
implementation is SystemVerilog; C++ provides host commands, the external
SDRAM chip model, an independent SRR oracle and SDL2 presentation. SDL2 is
optional for a headless RTL run.

### Windows RTL simulator

Open **MSYS2 UCRT64** from the Start menu and return to this checkout. The
normal SRR and RTL simulator have different prerequisites: SDL2 alone does not
provide Verilator, and Git does not download system packages.

```bash
pacman -Syu
```

If MSYS2 asks you to close the terminal, close it, reopen **MSYS2 UCRT64** and
run `pacman -Syu` again. Repeat until it reports that the system is current.
Then run:

```bash
git pull --ff-only
./scripts/setup-rtl-sim.sh
make rtl-gpu-run
```

The setup installs or reuses `make`, MSYS2 Perl, and the UCRT64 packages for GCC,
its `gcc-libs`/`cc-libs` compatibility packages, Python, pkgconf, SDL2 and Verilator. The
[official MSYS2 Verilator package](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-verilator?repo=ucrt64)
provides an extensionless Perl launcher as well as native executables. The
build helper resolves that launcher through MSYS2's Perl. Do not replace it
with an unrelated Windows Perl installation or mix compilers from TDM-GCC,
Git Bash or Visual Studio into this toolchain.

If installing GCC still reports a conflict between `libgcc` and older
`gcc-libs`/`cc-libs` after a full `pacman -Syu`, explicitly refresh and upgrade
both compatibility packages in the same full transaction:

```bash
pacman -Syyu mingw-w64-ucrt-x86_64-gcc-libs mingw-w64-ucrt-x86_64-cc-libs
```

The current [gcc-libs](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-gcc-libs)
and [cc-libs](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-cc-libs)
packages retain the legacy package names and depend on the split runtime
libraries. They are upgraded together; do not remove them or bypass dependency
checks. Restart UCRT64 again if that transaction asks, then retry the setup.
The setup does not perform a full system upgrade automatically and prints
these recovery commands if dependency installation fails.

Use `./scripts/setup-rtl-sim.sh --check` to verify existing tools without
installing anything. The check compiles and runs a small RTL/C++/SDL2 smoke
test. `make rtl-gpu` builds only; `make rtl-gpu-run` builds and launches the
window. The first build compiles the generated C++ and can take several
minutes. Build progress and periodic activity messages are printed; full
output is retained in `build/rtl-gpu/verilator-build.log`. For every compiler
message in the terminal, run `python -u scripts/gpu-rtl.py --verbose`.

After building, the equivalent direct commands are:

```bash
./build/nano-gpu-rtl.exe --scene showcase
./build/nano-gpu-rtl.exe --headless --scene showcase --frames 1 --verify
```

If a command fails, paste its complete output. These commands also identify
which installation and tools that terminal is actually using:

```bash
type -a make python perl verilator g++
cygpath -w /
./scripts/setup-rtl-sim.sh --check
```

The setup checks prerequisites; a successful dependency check alone is not
evidence that a complete Windows scene regression has passed. Debian/Ubuntu
under WSL remains an alternative using the Linux setup below. Rebuild on the
selected platform instead of copying the cloud's Linux executable to Windows.

Native Windows CI now builds the RTL simulator using the UCRT64 packages and
checks its SDL showcase against the SRR, followed by command/recovery tests.
Successful runs of the **Windows reference renderer** workflow provide a
`nano-gpu-rtl-windows` artifact containing the executable and runtime DLLs.
Extract the entire ZIP into one folder to run it without installing Verilator.
The package is also checked with the MSYS2 directories removed from PATH.

### Linux RTL simulator

On Linux x86_64, install the pinned, workspace-local OSS CAD Suite and build:

```bash
./scripts/setup-rtl-tools.sh
./scripts/setup-rtl-tools.sh --check
make rtl-gpu
./build/nano-gpu-rtl --headless --scene showcase --verify --stats build/rtl-showcase.json
```

The tool installer downloads OSS CAD Suite **2026-10-24**, verifies its pinned
SHA-256, and extracts it into the ignored `.deps/rtl` directory. It does not
modify system files. The suite provides Verilator, Icarus Verilog, Yosys and
nextpnr; building Verilator's generated C++ still needs the C++ compiler and
make described above. The approximately 749 MB tool archive is separate from
the SDL2 setup and is unnecessary for normal SRR builds.

When SDL2 is discoverable through pkg-config during `make rtl-gpu`, run
`./build/nano-gpu-rtl --scene showcase --verify` for a window. The window displays
the pixels emitted by RTL scanout. Without SDL2, the same build produces a
headless executable; pass `--headless` when running it. To require a headless
build explicitly:

```bash
python3 scripts/gpu-rtl.py --headless-only
```

Additional targets:

```bash
make rtl-gpu-test
make rtl-block-test
make rtl-gpu-synth
```

The first runs command/recovery checks and integrated scene comparisons against
the independent renderer. `rtl-block-test` runs the separate native RAM/video,
asynchronous host, tile-renderer, SDRAM and texture-cache regressions. The last
target synthesizes the functional core for Gowin and reports retained cells;
synthesis alone does not qualify the complete GPU's physical fit or clock.
See the [RTL guide](hardware/rtl-gpu.md) for generated reports and the distinction
between the integrated GPU and the older UV arithmetic probe.

The full pinned OSS CAD Suite bootstrap in `setup-rtl-tools.sh` remains Linux
x86_64 only. Windows simulation uses the native MSYS2 package through
`setup-rtl-sim.sh`; it does not need a Windows OSS CAD Suite installation.
Synthesis and routing on other platforms still require those separate HDL
tools. Set `NANO_GPU_RTL_TOOLS` to a custom suite root or put the required tools
on PATH. Rebuild generated simulators on each machine rather than copying
binaries or `.deps` installations between platforms.


### Full Tang Nano 20K target

The Linux x86_64 pinned HDL suite also builds the actual hardware-3921 target:

```bash
make tang20k-test
make tang20k
```

The first checks board pad/clock behavior and the real GPU at its fixed board
profile. The second repeats or reuses source-matched functional checks, then
synthesizes, packs, places/routes, verifies fixed SiP connections and checks
internal clock timing before emitting `build/tang20k/nano-gpu.fs`. It removes
a previous image before starting, so a failed run cannot leave a stale image
at that output path. Inspect `build/tang20k/board-report.json` and logs on failure.

This target uses a 50.4 MHz core, 25.2 MHz pixels and a 126 MHz DDR serializer.
The default logical-core simulator retains its separate nominal 100.8 MHz
profile. To run the real-core simulator at the board ratios manually:

```bash
python3 scripts/gpu-rtl.py --board-core --build-dir build/tang20k-core-sim --output build/nano-gpu-tang20k-sim
./build/nano-gpu-tang20k-sim --headless --verify --scene showcase --frames 2
```

Board wrapper tests use functional PLL/divider/pad models, not analog timing
models. Full-core tests use the real GPU RTL and the external SDRAM model;
they do not link C++ GPU backends. External setup/hold margins and the received
physical board still require hardware tests. There is no built-in command
player and no RP2350 PIO/DMA driver in this target. Programming and the existing
wiring/reset instructions are in the two top-level GPU guides. The image can
be programmed from Windows with openFPGALoader; rebuilding it requires the
separate synthesis, placement/routing and packing tools, beyond the Windows
simulator dependencies.
