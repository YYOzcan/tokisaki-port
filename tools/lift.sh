#!/usr/bin/env bash
# Tokisaki Port -- regenerate the lifted Killzone 2 code from your own EBOOT.ELF.
#
#   tools/lift.sh [path/to/EBOOT.ELF]
#
# Writes out/ (loader metadata) and recompiled/ (C++). Both are game-derived
# and gitignored; never commit them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="${1:-${ROOT}/game_dump/PS3_GAME/USRDIR/EBOOT.ELF}"
TOOLS="${ROOT}/external/ps3recomp/tools"
PY="${ROOT}/.venv/bin/python"

if [ ! -x "${PY}" ]; then
    python3 -m venv "${ROOT}/.venv"
    "${PY}" -m pip install -q -r "${ROOT}/external/ps3recomp/requirements.txt"
fi

# End of the last executable section (.sceStub.text) in BCUS98116 v1.00.
# Anything branched to above it is data, not code.
CODE_END=0xbba45c

mkdir -p "${ROOT}/out" "${ROOT}/recompiled"
"${PY}" -I "${TOOLS}/ppu_loader.py" "${ELF}" -o "${ROOT}/out/"
rm -f "${ROOT}"/recompiled/ppu_recomp_*.cpp
"${PY}" -I "${TOOLS}/ppu_lifter.py" "${ELF}" \
    --functions "${ROOT}/out/EBOOT.functions.json" \
    --hle-stubs "${ROOT}/out/EBOOT.imports.json" \
    --code-end "${CODE_END}" \
    --chunk-lines 150000 \
    -j "${LIFT_JOBS:-8}" \
    -o "${ROOT}/recompiled/"
grep -ho "^void func_[0-9A-F]*(" "${ROOT}"/recompiled/ppu_recomp_*.cpp | sed "s/void func_//; s/(//" | sort -u > "${ROOT}/out/lifted_funcs.txt"
"${PY}" -I "${ROOT}/tools/postlift_fixups.py"   # source fixups to the generated C (see the script)
