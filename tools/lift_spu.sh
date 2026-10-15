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
    --symbol-prefix pm_DDD59DC528B4CED6_ -o "${SPU}/pm_DDD59DC528B4CED6"

"${PY}" -I "${ROOT}/tools/extract_jobmods_elf.py" "${ELF}" "${OUT}/jobmods"
"${PY}" -I "${T}/lift_jobmods.py" --jobmods "${OUT}/jobmods" --out "${SPU}" \
    --register "${SPU}/jobmods_register.c" --symbol-prefix kz2
