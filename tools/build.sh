#!/usr/bin/env bash
# Tokisaki Port -- configure and build (Linux, clang, Vulkan backend).
#   BUILD_JOBS=4 tools/build.sh
# Each lifted chunk needs ~2 GB of RAM to compile; keep BUILD_JOBS within
# (free RAM / 2 GB).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ ! -f "${ROOT}/build/build.ninja" ]; then
    cmake -S "${ROOT}" -B "${ROOT}/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DPS3RECOMP_RSX_VULKAN=ON
fi
nice -n 10 cmake --build "${ROOT}/build" -j "${BUILD_JOBS:-4}"
