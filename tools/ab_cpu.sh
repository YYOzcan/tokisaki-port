#!/usr/bin/env bash
# Tokisaki Port -- alternating runs of two or more builds at the Corinth River
# checkpoint (run_bench_tpl/), measuring the game's CPU time per presented
# frame between 100 s and 155 s in, besides the frame rate. The frame is
# CPU-bound, so CPU ms per frame tracks the work a change saves even when
# other programs on the desktop make the frame rate itself noisy.
#   tools/ab_cpu.sh <tag> <rounds> <build dir> <build dir> [...]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.."; R="$PWD"
export PS3_AUDIO_MUTE="${PS3_AUDIO_MUTE:-1}"
tag=$1; n=$2; shift 2
HZ=$(getconf CLK_TCK)
sample() {   # <pid> <log>: cpu ticks and frames presented so far
    local t f
    t=$(awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null)
    f=$(grep -a '\[fps\]' "$2" | tail -1 | sed -E 's/.*\(([0-9]+) total\).*/\1/')
    echo "${t:-0} ${f:-0}"
}
for r in $(seq 1 "$n"); do for b in "$@"; do
  L="logs/abc_${tag}_${b}_$r.log"
  KZ2_RUN_DIR="$R/run_bench" KZ2_RUN_TEMPLATE="$R/run_bench_tpl" KZ2_BUILD_DIR="$R/$b" tools/longrun.sh 160 "$L" > /dev/null 2>&1 &
  LR=$!
  sleep 100; G=$(pgrep -n -x killzone2); read -r t0 f0 <<< "$(sample "$G" "$L")"; s0=$(date +%s.%N)
  sleep 55;  read -r t1 f1 <<< "$(sample "$G" "$L")"; s1=$(date +%s.%N)
  wait "$LR"
  awk -v b="$b" -v r="$r" -v t0="$t0" -v t1="$t1" -v f0="$f0" -v f1="$f1" -v s0="$s0" -v s1="$s1" -v hz="$HZ" \
    'BEGIN { df = f1 - f0; if (df <= 0) { printf "%s run %s: NO-FRAMES\n", b, r; exit }
             printf "%s run %s: %.2f fps, %.1f CPU ms/frame (%.2f cores)\n", b, r, df / (s1 - s0),
                    (t1 - t0) * 1000 / hz / df, (t1 - t0) / hz / (s1 - s0) }'
done; done
