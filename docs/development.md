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
already provide a C++ compiler. Windows users can use Debian/Ubuntu under WSL.
Native Windows installation is not automated by this script.

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
