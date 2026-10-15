#!/usr/bin/env bash
# Tokisaki Port -- apply this port's fixes to the pinned ps3recomp submodule.
# Idempotent: a patch that is already applied is skipped.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ROOT}/external/ps3recomp"
for p in "${ROOT}"/patches/ps3recomp/*.patch; do
    if git -C "${SDK}" apply --check "${p}" 2>/dev/null; then
        git -C "${SDK}" apply "${p}" && echo "[patch] applied  $(basename "${p}")"
    elif git -C "${SDK}" apply --reverse --check "${p}" 2>/dev/null; then
        echo "[patch] present  $(basename "${p}")"
    else
        echo "[patch] CONFLICT $(basename "${p}")" >&2; exit 1
    fi
done
