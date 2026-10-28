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
    --extra-funcs 0xA2C,0xA84,0xA90,0xAD0 --merge-chunks
# 0xA2C is the PM's SPU interrupt vector (MFC list stall-and-notify while a
# job is being loaded); 0xA84/0xA90/0xAD0 are where it returns from the
# register save/restore stub it generates at LS 0x3FEC0. Nothing branches to
# them statically, so without these entries the whole handler ran in the
# fallback interpreter and the PM ended mid-handler: no job ever started.
# Merged chunks: its wait loops run as gotos instead of a trampoline and a
# function lookup per branch (PROGRESS 75).

"${PY}" -I "${ROOT}/tools/extract_jobmods_elf.py" "${ELF}" "${OUT}/jobmods"
# Entries the jobs reach only through computed branches, found with the
# runtime's SPU_INTERP_ENTRY_LOG=1 (they ran in the fallback interpreter --
# the post-processing module 9de83b69cfa0 spent most of its time there).
# 73d064721abc / 8e97047fac2a / eb97616444b5: entries reached only through
# function pointers the job builds (SPU_INTERP_ENTRY_LOG at LS 0x8400: 0x84C0,
# 0x8640, 0xBAE8), given at the job code buffer's base and moved with JOBMOD_ALT_BASE.
export JOBMOD_EXTRA_FUNCS="${JOBMOD_EXTRA_FUNCS:-9de83b69cfa0:0x4600,0x5300,0x8808;73d064721abc:0x40C0,0x4240,0x76E8;8e97047fac2a:0x40C0,0x4240;eb97616444b5:0x40C0,0x4240;15e56deeec92:0x5050,0x5154,0x4C88,0x4868,0x4620,0x4660,0x4720,0x4DC8,0x50A0,0x5228,0x4D90}"
# Every module lifted with merged chunks (one C function per run of chunks,
# loops as gotos, instead of a trampoline at every branch target). It
# livelocked until SPU_RdMachStat's interrupts-on assertion check got the
# instruction's own address (PROGRESS 71). JOBMOD_LIFTER_ARGS= (empty) lifts
# chunk by chunk; JOBMOD_MERGE="<sha>,..." then merges only those modules.
export JOBMOD_LIFTER_ARGS="${JOBMOD_LIFTER_ARGS---merge-chunks}"
export JOBMOD_MERGE="${JOBMOD_MERGE-}"
# The depth-of-field / motion-blur kernels of the two jobs that build the
# blur layer the port zeroes (PROGRESS 68, 71): entries that return at once
# while the runtime's g_spu_skip_marked is set (boot_main.cpp, unless
# KZ2_POST_BLUR=1). Each is called from the job body as `brsl $r0` and does
# no channel I/O; the body still takes its buffers and DMAs the layer out.
export JOBMOD_SKIPPABLE="${JOBMOD_SKIPPABLE-a27e818cca34:0x4A20,0x4CB0,0x5138,0x5458;e569adaf4ffe:0x4A20,0x4C40,0x50C8,0x53E8}"
# Three modules the game keeps at LS 0x8400 beside the job code buffer's job
# (73d064721abc, the always-running one, 8e97047fac2a, eb97616444b5): lifted
# there too, so they run compiled instead of interpreted (PROGRESS 84).
export JOBMOD_ALT_BASE="${JOBMOD_ALT_BASE-73d064721abc@0x8400,8e97047fac2a@0x8400,eb97616444b5@0x8400}"
"${PY}" -I "${T}/lift_jobmods.py" --jobmods "${OUT}/jobmods" --out "${SPU}" \
    --register "${SPU}/jobmods_register.c" --symbol-prefix kz2

# Register-cached copies (spu_recomp_rc.c, built instead of spu_recomp.c with
# CMake's KZ2_SPU_REGCACHE=ON): KZ2_SPU_REGCACHE=1 tools/lift_spu.sh. Slower
# on this title so far (PROGRESS 62).
if [ "${KZ2_SPU_REGCACHE:-0}" = 1 ]; then
    "${PY}" -I "${ROOT}/tools/spu_regcache.py" "${SPU}" | tail -1
fi
