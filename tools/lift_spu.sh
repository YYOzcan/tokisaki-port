#!/usr/bin/env bash
# Tokisaki Port -- lift Killzone 2's SPU code into spu_recompiled/ (gitignored).
#
#  1. the 23 SPU ELF images embedded in the EBOOT (SPURS tasks, raw SPU threads)
#  2. the WWS job manager policy module (raw, guest 0xD8B000, LS 0xA00)
#  3. the 36 WWS "C0DEC0DE" job modules (LS 0x4000 overlays)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="${1:-${ROOT}/game_dump/PS3_GAME/USRDIR/EBOOT.ELF}"
T="${ROOT}/external/ps3recomp/tools"
PY="${ROOT}/.venv/bin/python"
OUT="${ROOT}/out"
SPU="${ROOT}/spu_recompiled"
mkdir -p "${OUT}/spu_images" "${OUT}/spu_pm" "${OUT}/jobmods" "${SPU}"

"${PY}" -I "${T}/extract_spu_images.py" "${ELF}" --output "${OUT}/spu_images"
"${PY}" -I "${T}/build_spu_workloads.py" --images "${OUT}/spu_images" --lifted "${SPU}" \
    --out "${SPU}/spu_workloads.c" --register-fn kz2_spu_register_all --constructor --title killzone2

"${PY}" -I - "${ELF}" "${OUT}/spu_pm/wws_pm.bin" <<'PYEOF'
import sys
d = open(sys.argv[1], "rb").read()
off = 0xD8B000 - 0x10000            # PT_LOAD 0: vaddr 0x10000 at file offset 0
open(sys.argv[2], "wb").write(d[off:off + 0x2E80])
PYEOF
"${PY}" -I "${T}/wrap_spu_elf.py" "${OUT}/spu_pm/wws_pm.bin" --base 0xA00 --entry 0xA00 \
    --out "${OUT}/spu_pm/pm_DDD59DC528B4CED6.elf"
"${PY}" -I "${T}/spu_lifter.py" --auto-functions "${OUT}/spu_pm/pm_DDD59DC528B4CED6.elf" \
    --symbol-prefix pm_DDD59DC528B4CED6_ -o "${SPU}/pm_DDD59DC528B4CED6" \
    --extra-funcs 0xA2C,0xA84,0xA90,0xAD0
# 0xA2C is the PM's SPU interrupt vector (MFC list stall-and-notify while a
# job is being loaded); 0xA84/0xA90/0xAD0 are where it returns from the
# register save/restore stub it generates at LS 0x3FEC0. Nothing branches to
# them statically, so without these entries the whole handler ran in the
# fallback interpreter and the PM ended mid-handler: no job ever started.

"${PY}" -I "${ROOT}/tools/extract_jobmods_elf.py" "${ELF}" "${OUT}/jobmods"
# Entries the jobs reach only through computed branches, found with the
# runtime's SPU_INTERP_ENTRY_LOG=1 (they ran in the fallback interpreter --
# the post-processing module 9de83b69cfa0 spent most of its time there).
export JOBMOD_EXTRA_FUNCS="${JOBMOD_EXTRA_FUNCS:-9de83b69cfa0:0x4600,0x5300,0x8808}"
"${PY}" -I "${T}/lift_jobmods.py" --jobmods "${OUT}/jobmods" --out "${SPU}" \
    --register "${SPU}/jobmods_register.c" --symbol-prefix kz2

# Register-cached copies (spu_recomp_rc.c, built instead of spu_recomp.c with
# CMake's KZ2_SPU_REGCACHE=ON): KZ2_SPU_REGCACHE=1 tools/lift_spu.sh. Slower
# on this title so far (PROGRESS 62).
if [ "${KZ2_SPU_REGCACHE:-0}" = 1 ]; then
    "${PY}" -I "${ROOT}/tools/spu_regcache.py" "${SPU}" | tail -1
fi
