#!/usr/bin/env bash
# Tokisaki Port -- turn the submodule's uncommitted changes that are NOT yet in
# patches/ps3recomp/ into the next patch of the series.
#   tools/make_patch.sh <name>        e.g. tools/make_patch.sh spu-float-tier1
# Builds a scratch worktree of the pinned commit, applies the existing series
# there, copies every file the working submodule changed (tracked and new),
# and writes the difference as patches/ps3recomp/NNNN-<name>.patch.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${ROOT}/external/ps3recomp"
NAME="${1:?patch name}"
BASE="$(git -C "${ROOT}" ls-tree HEAD external/ps3recomp | awk '{print $3}')"
TMP="$(mktemp -d)"
trap 'git -C "${SDK}" worktree remove --force "${TMP}/wt" >/dev/null 2>&1 || true; rm -rf "${TMP}"' EXIT
git -C "${SDK}" worktree add --detach "${TMP}/wt" "${BASE}" >/dev/null 2>&1
for p in "${ROOT}"/patches/ps3recomp/*.patch; do git -C "${TMP}/wt" apply "${p}"; done
git -C "${TMP}/wt" add -A >/dev/null
git -C "${TMP}/wt" -c user.email=x -c user.name=x commit -qm series
( cd "${SDK}" && { git diff --name-only; git ls-files --others --exclude-standard; } ) | sort -u | while read -r f; do
    mkdir -p "$(dirname "${TMP}/wt/${f}")"; cp -p "${SDK}/${f}" "${TMP}/wt/${f}"
done
git -C "${TMP}/wt" add -A
N="$(ls "${ROOT}"/patches/ps3recomp/*.patch | wc -l)"
OUT="${ROOT}/patches/ps3recomp/$(printf '%04d' $((N + 1)))-${NAME}.patch"
git -C "${TMP}/wt" diff --cached > "${OUT}"
if [ ! -s "${OUT}" ]; then rm -f "${OUT}"; echo "no changes beyond the series"; exit 0; fi
echo "${OUT}: $(grep -c '^diff --git' "${OUT}") files"
