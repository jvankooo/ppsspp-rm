#!/usr/bin/env bash
# Local wrapper: builds the Switch NRO with the Mesa SDK available on this PC.
# Run from the devkitPro MSYS2 shell:  C:\devkitPro\msys2\usr\bin\bash -l build-rumble.sh
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
export SWITCH_MESA_VERSION="${SWITCH_MESA_VERSION:-26.2.3}"
export SWITCH_MESA_PREFIX="${SWITCH_MESA_PREFIX:-$ROOT/build-switch-deps/mesa-$SWITCH_MESA_VERSION-sdk/opt/devkitpro/portlibs/switch}"
export UAM_PYTHON="${UAM_PYTHON:-/c/msys64/mingw64/bin/python.exe}"
export PYTHON="$UAM_PYTHON"
# Mesa SDK note: its CMake configs ask for libelf, which no Mesa library uses. Give it an empty one.
MESA_LIB="$SWITCH_MESA_PREFIX/lib"
[[ -f "$MESA_LIB/libelf.a" ]] || /opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar rcs "$MESA_LIB/libelf.a"
# bison/flex/m4 and the host python3 (glslang) come from the separate MSYS2 install (appended, so devkitPro tools win).
export PATH="$PATH:/c/msys64/usr/bin:/c/msys64/mingw64/bin"
export BISON_PKGDATADIR="${BISON_PKGDATADIR:-/c/msys64/usr/share/bison}"
export M4="${M4:-/c/msys64/usr/bin/m4}"
export JOBS="${JOBS:-12}"
exec bash "$ROOT/scripts/build-switch.sh" "$@"
