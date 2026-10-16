#!/usr/bin/env bash
# Tokisaki Port -- build run/vfs_nointro: a VFS root of symlinks to the dump
# with MOVIES/GameIntro_* left out. The intro movie's player still deadlocks
# (see docs/PROGRESS.md); without the file the front end skips it and goes on
# to the start screen. Nothing in game_dump is modified.
#   tools/make_vfs_nointro.sh && PS3_VFS_ROOT=$PWD/run/vfs_nointro tools/run.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/game_dump/PS3_GAME"
DST="${ROOT}/run/vfs_nointro/PS3_GAME"
mkdir -p "${DST}/USRDIR/MOVIES"
for f in "${SRC}"/*; do b=$(basename "$f"); [ "$b" = USRDIR ] || ln -sfn "$f" "${DST}/$b"; done
for f in "${SRC}"/USRDIR/*; do b=$(basename "$f"); [ "$b" = MOVIES ] || ln -sfn "$f" "${DST}/USRDIR/$b"; done
for f in "${SRC}"/USRDIR/MOVIES/*; do
    b=$(basename "$f")
    case "$b" in GameIntro*) ;; *) ln -sfn "$f" "${DST}/USRDIR/MOVIES/$b" ;; esac
done
echo "run/vfs_nointro ready"
