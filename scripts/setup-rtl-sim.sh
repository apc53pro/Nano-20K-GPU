#!/usr/bin/env bash
# Install the simulator prerequisites, then verify the actual selected tools.
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
check_only=false

usage() {
    cat <<'USAGE'
Usage: scripts/setup-rtl-sim.sh [--check]

Set up the Verilator + SDL2 GPU simulator. Windows uses native MSYS2 UCRT64
packages; Linux uses the existing SDL2 setup and pinned local OSS CAD Suite.
This does not build the complete GPU or open its window.

  --check    Verify existing prerequisites without installing anything.
  --help     Show this help.

After setup: make rtl-gpu-run

Before installing Windows packages, update MSYS2 with pacman -Syu. If it asks
you to close the terminal, reopen MSYS2 UCRT64 and run pacman -Syu again.
USAGE
}

die() { printf 'setup-rtl-sim: %s\n' "$*" >&2; exit 1; }

upgrade_help() {
    printf '%s\n' \
        'MSYS2 needs a complete, consistent package upgrade before installing these dependencies.' \
        'Run: pacman -Syu' \
        'If asked to close the terminal, close it, reopen MSYS2 UCRT64 and run pacman -Syu again.' \
        'If the GCC runtime conflict persists after that full upgrade, refresh and upgrade both compatibility packages in the same full transaction:' \
        'pacman -Syyu mingw-w64-ucrt-x86_64-gcc-libs mingw-w64-ucrt-x86_64-cc-libs' \
        'Then rerun ./scripts/setup-rtl-sim.sh.' \
        'Do not remove gcc-libs or bypass dependency checks; the upgrade updates matching runtime libraries.' >&2
}

while (($#)); do
    case "$1" in
        --check) check_only=true ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

system="$(uname -s)"
case "$system" in
    MINGW*|MSYS*|CYGWIN*)
        [[ "${MSYSTEM:-}" == UCRT64 ]] ||
            die 'open MSYS2 UCRT64 from the Start menu, then rerun this script'
        package_manager="$(command -v pacman || true)"
        if [[ -z "$package_manager" && -x /usr/bin/pacman.exe ]]; then
            package_manager=/usr/bin/pacman.exe
        fi
        if [[ -z "$package_manager" ]]; then
            printf '%s\n' \
                'setup-rtl-sim: pacman was not found; this is not a complete MSYS2 installation.' \
                'Install MSYS2 from https://www.msys2.org/ and launch MSYS2 UCRT64.' \
                'Git Bash is a separate installation, even if its prompt says UCRT64.' >&2
            if command -v cygpath >/dev/null 2>&1; then
                printf 'Current shell installation root: %s\n' "$(cygpath -w /)" >&2
            fi
            exit 1
        fi
        if ! $check_only; then
            packages=(make perl
                mingw-w64-ucrt-x86_64-gcc
                mingw-w64-ucrt-x86_64-gcc-libs
                mingw-w64-ucrt-x86_64-cc-libs
                mingw-w64-ucrt-x86_64-python
                mingw-w64-ucrt-x86_64-pkgconf
                mingw-w64-ucrt-x86_64-SDL2
                mingw-w64-ucrt-x86_64-verilator)
            if "$package_manager" -Q "${packages[@]}" >/dev/null 2>&1; then
                printf 'Reusing installed MSYS2 UCRT64 simulator packages.\n'
            else
                # Reuse the local package database without synchronizing it.
                # A partial rolling-release upgrade can mix incompatible GCC
                # runtimes; a full system upgrade remains a separate user step.
                if pending_upgrades="$("$package_manager" -Qu 2>&1)"; then
                    if [[ -n "$pending_upgrades" ]]; then
                        printf 'setup-rtl-sim: package upgrades are pending; no packages were installed.\n' >&2
                        upgrade_help
                        exit 1
                    fi
                else
                    query_status=$?
                    # pacman returns 1 with no output when no updates exist.
                    if [[ "$query_status" != 1 || -n "$pending_upgrades" ]]; then
                        printf 'setup-rtl-sim: could not check package upgrades: %s\n' "$pending_upgrades" >&2
                        exit "$query_status"
                    fi
                fi
                printf 'Installing Verilator, SDL2, Python and native C++ build prerequisites.\n'
                if "$package_manager" -S --needed --noconfirm "${packages[@]}"; then
                    :
                else
                    install_status=$?
                    printf 'setup-rtl-sim: package installation failed (status %s).\n' "$install_status" >&2
                    upgrade_help
                    exit "$install_status"
                fi
                hash -r
            fi
        fi
        python_command="${PYTHON:-python}"
        ;;
    Linux)
        python_command="${PYTHON:-python3}"
        if ! $check_only; then
            bash "$project_dir/scripts/setup-sdl2.sh"
            PYTHON="$python_command" bash "$project_dir/scripts/setup-rtl-tools.sh"
        fi
        ;;
    *)
        python_command="${PYTHON:-python3}"
        $check_only || die 'automatic setup supports Linux and MSYS2 UCRT64; install Verilator, SDL2, Python 3 and a C++17 compiler, then rerun with --check'
        ;;
esac

command -v "$python_command" >/dev/null 2>&1 ||
    die "Python 3 was not found ($python_command); rerun setup without --check"

printf 'Checking the simulator toolchain, C++ support and SDL2...\n'
"$python_command" -u "$project_dir/scripts/gpu-rtl.py" --check --require-sdl
printf '%s\n' \
    'Simulator prerequisites are ready.' \
    'Run make rtl-gpu-run to build the simulator and open its SDL2 window.'
