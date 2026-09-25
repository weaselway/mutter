#!/usr/bin/env bash
# Builds mutter the way weaselway ships it, without installing anything.
# See WEASELWAY.md. Runs itself inside `nix develop` if not already there.
#
#   ./weaselway-build.sh              configure on first run, then compile
#   RECONFIGURE=1 ./weaselway-build.sh  re-apply the flags below to an existing build dir
#   BUILDTYPE=debugoptimized ./weaselway-build.sh
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -z "${IN_NIX_SHELL:-}" ]; then
    exec nix develop "${SOURCE_DIR}" -c "$0" "$@"
fi

BUILD_DIR="${BUILD_DIR:-${SOURCE_DIR}/_build/nix}"
BUILDTYPE="${BUILDTYPE:-release}"

# Same flags as weaselway/dev/build-mutter.sh, minus prefix/libdir (no install).
MESON_FLAGS=(
    -Dbuildtype="${BUILDTYPE}"
    -Drdp=enabled
    -Dtests=disabled
    -Ddocs=false
    -Dprofiler=false
    -Dcogl_tests=false
    -Dclutter_tests=false
    -Dmutter_tests=false
    -Dinstalled_tests=false
    -Dintrospection=true
)

if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    meson setup "${BUILD_DIR}" "${SOURCE_DIR}" "${MESON_FLAGS[@]}"
elif [ -n "${RECONFIGURE:-}" ]; then
    meson setup --reconfigure "${BUILD_DIR}" "${SOURCE_DIR}" "${MESON_FLAGS[@]}"
fi

meson compile -C "${BUILD_DIR}"
