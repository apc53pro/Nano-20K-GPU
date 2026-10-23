#!/usr/bin/env bash
# Install the SDL2 development environment and verify the RGB565 display path.
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="$project_dir/.deps/sdl2"
local_install=false
check_only=false
rebuild=false

usage() {
    cat <<'USAGE'
Usage: scripts/setup-sdl2.sh [--local] [--prefix DIR] [--rebuild] [--check]

Install native dependencies with MSYS2 on Windows, or reuse/install an SDL2 SDK
on Debian/Ubuntu or macOS/Homebrew. Without root/sudo on Linux, build a pinned
SDL2 release into .deps/sdl2 instead. Local builds do not change system files.

  --local       Build/reuse a local SDL2 SDK on Linux (no root required).
  --prefix DIR  Choose its installation directory; implies --local.
  --rebuild     Rebuild the local SDK, including after moving/interrupted setup.
  --check       Verify the selected/existing SDK without installing packages.
  --help        Show this help.

Local builds require a C/C++ compiler, make, pkg-config, curl, tar, and a SHA-256
tool. CXX may name the C++ compiler executable; JOBS controls build parallelism.
USAGE
}

die() { printf 'setup-sdl2: %s\n' "$*" >&2; exit 1; }

while (($#)); do
    case "$1" in
        --local) local_install=true ;;
        --prefix)
            (($# >= 2)) && [[ -n "$2" && "$2" != --* ]] || die '--prefix requires a directory'
            prefix="$2"; local_install=true; shift ;;
        --check) check_only=true ;;
        --rebuild) rebuild=true; local_install=true ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

system="$(uname -s)"
case "$system" in
    MINGW*|MSYS*|CYGWIN*)
        command -v pacman >/dev/null 2>&1 ||
            die 'use an MSYS2 UCRT64 terminal, not Git Bash; see docs/development.md'
        case "${MSYSTEM:-}" in
            UCRT64) package_prefix=mingw-w64-ucrt-x86_64; compiler_package=gcc ;;
            MINGW64) package_prefix=mingw-w64-x86_64; compiler_package=gcc ;;
            CLANG64) package_prefix=mingw-w64-clang-x86_64; compiler_package=clang ;;
            *) die 'open MSYS2 UCRT64 (recommended), MINGW64, or CLANG64; see docs/development.md' ;;
        esac
        ! $local_install || die '--local/--prefix/--rebuild are Linux-only; MSYS2 uses pacman packages'
        if ! $check_only; then
            # Install all build/test tools even if an SDL2 SDK is already present.
            printf 'Installing native dependencies for MSYS2 %s.\n' "$MSYSTEM"
            pacman -S --needed --noconfirm make \
                "$package_prefix-$compiler_package" "$package_prefix-pkgconf" \
                "$package_prefix-SDL2" "$package_prefix-python"
        fi
        exec bash "$project_dir/scripts/check-sdl2.sh"
        ;;
esac

use_local_sdk() {
    export SDL2_PREFIX="$prefix"
    export PATH="$prefix/bin:$PATH"
    export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export CMAKE_PREFIX_PATH="$prefix${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
}

sdk_available() {
    command -v "${CXX:-c++}" >/dev/null 2>&1 &&
        command -v pkg-config >/dev/null 2>&1 &&
        pkg-config --exists sdl2 &&
        [[ "$(pkg-config --modversion sdl2)" == 2.* ]]
}

# Reuse this checkout's local SDK without requiring a prior activation command.
if [[ -f "$prefix/lib/pkgconfig/sdl2.pc" ]]; then
    prefix="$(cd -- "$prefix" && pwd)"
    use_local_sdk
fi

if $check_only; then
    if $local_install && [[ ! -f "$prefix/lib/pkgconfig/sdl2.pc" ]]; then
        die "no local SDL2 SDK at $prefix"
    fi
    exec bash "$project_dir/scripts/check-sdl2.sh"
fi

if ! $rebuild && sdk_available && { ! $local_install || [[ -f "$prefix/lib/pkgconfig/sdl2.pc" ]]; }; then
    printf 'Reusing SDL2 %s.\n' "$(pkg-config --modversion sdl2)"
else
    if ! $local_install && [[ "$system" == Darwin ]]; then
        command -v brew >/dev/null 2>&1 || die 'install Homebrew first, then rerun this script'
        command -v "${CXX:-c++}" >/dev/null 2>&1 || die 'install Xcode Command Line Tools first'
        brew install pkg-config sdl2
    elif ! $local_install && [[ "$system" == Linux ]] && command -v apt-get >/dev/null 2>&1 &&
            { [[ "$(id -u)" == 0 ]] || command -v sudo >/dev/null 2>&1; }; then
        elevate=()
        if [[ "$(id -u)" != 0 ]]; then elevate=(sudo); fi
        "${elevate[@]}" apt-get update
        "${elevate[@]}" apt-get install -y --no-install-recommends build-essential pkg-config libsdl2-dev
    else
        [[ "$system" == Linux ]] || die 'local source builds are supported on Linux; see docs/development.md'
        for tool in "${CC:-cc}" "${CXX:-c++}" make pkg-config curl tar; do
            command -v "$tool" >/dev/null 2>&1 || die "local build requires $tool; install the prerequisites in docs/development.md"
        done
        if command -v sha256sum >/dev/null 2>&1; then
            hash_command=(sha256sum)
        elif command -v shasum >/dev/null 2>&1; then
            hash_command=(shasum -a 256)
        else
            die 'local build requires sha256sum or shasum'
        fi
        [[ "$prefix" != *[[:space:]]* ]] || die 'choose a --prefix without whitespace (SDL configure/pkg-config limitation)'
        jobs="${JOBS:-4}"
        [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer'
        mkdir -p -- "$prefix"
        prefix="$(cd -- "$prefix" && pwd)"
        [[ "$prefix" != *[[:space:]]* ]] || die 'choose an absolute --prefix without whitespace'
        [[ "$prefix" != / ]] || die 'choose a dedicated local prefix, not /'

        # Pin an official SDL2 release for repeatable local builds.
        sdl_version=2.32.4
        sdl_sha256=f15b478253e1ff6dac62257ded225ff4e7d0c5230204ac3450f1144ee806f934
        archive="$prefix/.cache/SDL2-$sdl_version.tar.gz"
        mkdir -p -- "$prefix/.cache"
        if [[ ! -f "$archive" ]]; then
            printf 'Downloading SDL2 %s.\n' "$sdl_version"
            curl --fail --location --show-error --silent --retry 3 --connect-timeout 20 --max-time 300 \
                --proto '=https' "https://www.libsdl.org/release/SDL2-$sdl_version.tar.gz" \
                --output "$archive.part"
            mv -- "$archive.part" "$archive"
        fi
        actual_hash="$("${hash_command[@]}" "$archive")"
        [[ "${actual_hash%% *}" == "$sdl_sha256" ]] || die "SHA-256 mismatch: remove $archive and retry"

        build_dir="$(mktemp -d "${TMPDIR:-/tmp}/nano-gpu-sdl2.XXXXXX")"
        trap 'rm -rf -- "$build_dir"' EXIT
        tar -xzf "$archive" -C "$build_dir"
        mkdir "$build_dir/build"
        printf 'Building SDL2 %s into %s (log: %s/setup.log).\n' "$sdl_version" "$prefix" "$prefix"
        if ! (
            cd "$build_dir/build"
            "$build_dir/SDL2-$sdl_version/configure" --prefix="$prefix" --libdir="$prefix/lib" --disable-static &&
            make -j "$jobs" &&
            make install
        ) >"$prefix/setup.log" 2>&1; then
            tail -n 50 "$prefix/setup.log" >&2
            die "build failed; see $prefix/setup.log"
        fi

        # Source this file in subsequent shells; do not modify shell startup files.
        {
            printf '# Generated by scripts/setup-sdl2.sh; source from Bash.\n'
            printf 'export SDL2_PREFIX=%q\n' "$prefix"
            printf 'export PATH=%q:"${PATH}"\n' "$prefix/bin"
            printf 'export PKG_CONFIG_PATH=%q"${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"\n' "$prefix/lib/pkgconfig"
            printf 'export LD_LIBRARY_PATH=%q"${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n' "$prefix/lib"
            printf 'export CMAKE_PREFIX_PATH=%q"${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"\n' "$prefix"
        } >"$prefix/env.sh"
        use_local_sdk
    fi
fi

if ! bash "$project_dir/scripts/check-sdl2.sh"; then
    die 'SDK verification failed; for a broken/moved local SDK, rerun with --rebuild (and the same --prefix if custom)'
fi
if [[ -f "$prefix/env.sh" ]]; then
    printf '\nFor future build shells, run:\n  source %q\n' "$prefix/env.sh"
fi
