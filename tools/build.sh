#!/usr/bin/env bash
# Tokisaki Port -- configure and build (Linux, clang, Vulkan backend).
#   BUILD_JOBS=4 tools/build.sh
# Each lifted chunk needs ~2 GB of RAM to compile; keep BUILD_JOBS within
# (free RAM / 2 GB).
#
#   KZ2_MAX=1 BUILD_JOBS=8 tools/build.sh
# builds for this machine only, at full strength: -march=native, the lifted
# PPU chunks at -O2 (~3 GB per job instead of ~2), and the lifted SPU code with
# the profile in pgo/spu.profdata when there is one. The binary does not run
# on an older CPU. KZ2_BUILD_DIR picks the build directory (default build/);
# CMAKE_ARGS passes anything else to the first configure, e.g. a Vulkan
# headers directory: CMAKE_ARGS=-DPS3RECOMP_VULKAN_INCLUDE_DIR=/path/include
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${KZ2_BUILD_DIR:-${ROOT}/build}"
EXTRA=()
if [ "${KZ2_MAX:-0}" = 1 ]; then
    EXTRA+=(-DKZ2_ARCH=native -DKZ2_RECOMP_COMPILE_OPTIONS=-O2)
    [ -f "${ROOT}/pgo/spu.profdata" ] && EXTRA+=(-DKZ2_SPU_PGO=use)
fi
if [ ! -f "${BUILD}/build.ninja" ]; then
    # shellcheck disable=SC2086
    cmake -S "${ROOT}" -B "${BUILD}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DPS3RECOMP_RSX_VULKAN=ON \
        -DPS3RECOMP_FFMPEG_RUNTIME=ON \
        "${EXTRA[@]}" ${CMAKE_ARGS:-}   # FFmpeg: cellVdec decodes the H.264 movies with the system FFmpeg
elif [ "${#EXTRA[@]}" -gt 0 ]; then
    cmake "${BUILD}" "${EXTRA[@]}" > /dev/null
fi
nice -n 10 cmake --build "${BUILD}" -j "${BUILD_JOBS:-4}"
