#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

export PS3="${HOME}/.ps3/data"
SCETOOL="${HOME}/.local/bin/scetool"

if [ ! -f "${SCETOOL}" ]; then
    echo "[-] scetool not found at ${SCETOOL}"
    exit 1
fi

INPUT="${1:-${PROJECT_ROOT}/game_dump/PS3_GAME/USRDIR/EBOOT.BIN}"
OUTPUT="${2:-${PROJECT_ROOT}/game_dump/PS3_GAME/USRDIR/EBOOT.ELF}"

echo "[*] Decrypting ${INPUT} -> ${OUTPUT}..."
"${SCETOOL}" -d "${INPUT}" "${OUTPUT}"
echo "[+] Decryption complete! Target ELF: ${OUTPUT}"
