#!/usr/bin/env bash
# Tokisaki Port -- alternating benchmark runs of two or more builds at the Corinth
# River checkpoint (run_bench_tpl/), the mean of the last 14 five-second windows.
#   tools/ab_builds.sh <tag> <rounds> <build dir> <build dir> [...]
# Runs are silent (PS3_AUDIO_MUTE=1) unless PS3_AUDIO_MUTE=0 is set.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."; R="$PWD"
export PS3_AUDIO_MUTE="${PS3_AUDIO_MUTE:-1}"
tag=$1; n=$2; shift 2
for r in $(seq 1 "$n"); do for b in "$@"; do
  L="logs/ab_${tag}_${b}_$r.log"
  KZ2_RUN_DIR="$R/run_bench" KZ2_RUN_TEMPLATE="$R/run_bench_tpl" KZ2_BUILD_DIR="$R/$b" tools/longrun.sh 160 "$L" > /dev/null 2>&1
  echo "$b run $r: $(grep -a '\[fps\]' "$L" | tail -14 | awk '{s+=$2} END {if (NR) printf "%.2f", s/NR; else print "NO-FPS"}') last=$(grep -a '\[fps\]' "$L" | tail -1 | awk '{print $2}') stops=$(grep -a -c 'synthesised stop' "$L")"
done; done
