#!/usr/bin/env bash
# Tokisaki Port -- decompile guest functions with Ghidra (project in out/ghidra,
# created once by the headless import, see docs/PROGRESS.md).
#   tools/decompile.sh 0x2B6460 0x3AC15C > logs/decomp.c
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GHIDRA="${GHIDRA_INSTALL_DIR:-/opt/ghidra}"
GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-4G}" "${GHIDRA}/support/analyzeHeadless" \
    "${ROOT}/out/ghidra" KZ2 -process "${KZ2_GHIDRA_PROGRAM:-EBOOT.ELF.0}" -noanalysis -readOnly \
    -scriptPath "${ROOT}/tools/ghidra" -postScript DecompileAt.java "$@" 2>&1 \
  | sed -n '/^INFO  DecompileAt.java> /{s/^INFO  DecompileAt.java> //;p;b};/^[^I]/p' \
  | grep -v "^WARN\|^INFO\|(GhidraScript)" || true
