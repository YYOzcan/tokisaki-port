#!/usr/bin/env bash
# Tokisaki Port -- alternating benchmark runs of one build under environment
# variants ("-" = none), as tools/ab_builds.sh measures them.
#   tools/ab_env.sh <tag> <rounds> <build dir> "VAR=1 VAR2=x" "-" [...]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."; R="$PWD"
export PS3_AUDIO_MUTE="${PS3_AUDIO_MUTE:-1}"
tag=$1; n=$2; B=$3; shift 3
for r in $(seq 1 "$n"); do k=0; for v in "$@"; do k=$((k+1))
  L="logs/abe_${tag}_${k}_$r.log"; E=(); [ "$v" != "-" ] && read -ra E <<< "$v"
  env "${E[@]}" KZ2_RUN_DIR="$R/run_bench" KZ2_RUN_TEMPLATE="$R/run_bench_tpl" KZ2_BUILD_DIR="$R/$B" tools/longrun.sh 160 "$L" > /dev/null 2>&1
  echo "[$v] run $r: $(grep -a '\[fps\]' "$L" | tail -14 | awk '{s+=$2} END {if (NR) printf "%.2f", s/NR; else print "NO-FPS"}') last=$(grep -a '\[fps\]' "$L" | tail -1 | awk '{print $2}') stops=$(grep -a -c 'synthesised stop' "$L")"
done; done
