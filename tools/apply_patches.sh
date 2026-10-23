#!/usr/bin/env bash
# Tokisaki Port -- apply this port's fixes to the pinned ps3recomp submodule.
# Idempotent: a patch that is already applied is skipped.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ROOT}/external/ps3recomp"
# A stacked series cannot be checked patch by patch once it is all applied
# (a later patch rewrites lines an earlier one added), so check the newest
# patch first: if it is in, the whole series is.
LAST="$(ls "${ROOT}"/patches/ps3recomp/*.patch | tail -1)"
if git -C "${SDK}" apply --reverse --check "${LAST}" 2>/dev/null; then
    echo "[patch] series present (up to $(basename "${LAST}"))"
    exit 0
fi
for p in "${ROOT}"/patches/ps3recomp/*.patch; do
    # Reverse check first: a hunk whose context occurs twice in the file (0008)
    # still passes the forward check once applied, and would land again.
    if git -C "${SDK}" apply --reverse --check "${p}" 2>/dev/null; then
        echo "[patch] present  $(basename "${p}")"
    elif git -C "${SDK}" apply --check "${p}" 2>/dev/null; then
        git -C "${SDK}" apply "${p}" && echo "[patch] applied  $(basename "${p}")"
    else
        echo "[patch] CONFLICT $(basename "${p}")" >&2; exit 1
    fi
done
