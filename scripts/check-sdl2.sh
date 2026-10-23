#!/usr/bin/env bash
# Compile and run a headless dependency check, not the GPU reference renderer.
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exe_suffix=""
default_cxx=c++
case "${MSYSTEM:-}" in
    UCRT64|MINGW64) exe_suffix=.exe ;;
    CLANG64) exe_suffix=.exe; default_cxx=clang++ ;;
esac
if [[ -z "$exe_suffix" && -z "${SDL2_PREFIX:-}" && -f "$project_dir/.deps/sdl2/env.sh" ]]; then
    source "$project_dir/.deps/sdl2/env.sh"
fi

cxx="${CXX:-$default_cxx}"
command -v "$cxx" >/dev/null 2>&1 || { printf 'C++ compiler not found: %s\n' "$cxx" >&2; exit 1; }
command -v pkg-config >/dev/null 2>&1 || { printf 'pkg-config is required; run scripts/setup-sdl2.sh\n' >&2; exit 1; }
pkg-config --exists sdl2 || { printf 'SDL2 development files not found; run scripts/setup-sdl2.sh\n' >&2; exit 1; }
[[ "$(pkg-config --modversion sdl2)" == 2.* ]] || { printf 'SDL2 major version 2 is required.\n' >&2; exit 1; }

build_dir="$(mktemp -d "${TMPDIR:-/tmp}/nano-gpu-sdl2-check.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT
read -r -a sdl_cflags <<<"$(pkg-config --cflags sdl2)"
read -r -a sdl_libs <<<"$(pkg-config --libs sdl2)"
"$cxx" -std=c++17 -Wall -Wextra -pedantic "${sdl_cflags[@]}" \
    "$project_dir/tools/sdl2-smoke.cpp" -o "$build_dir/sdl2-smoke$exe_suffix" "${sdl_libs[@]}"
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software "$build_dir/sdl2-smoke$exe_suffix"
