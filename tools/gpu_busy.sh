#!/usr/bin/env bash
# Tokisaki Port -- sample the AMD GPU's busy percentage and shader clock
# every 0.2 s for SECS seconds and print the average (amdgpu sysfs).
#   tools/gpu_busy.sh [secs=10]
SECS="${1:-10}"
DEV="$(for d in /sys/class/drm/card*/device; do [ -r "$d/gpu_busy_percent" ] && echo "$d" && break; done)"
[ -n "${DEV}" ] || { echo "no amdgpu gpu_busy_percent"; exit 1; }
n=0; sum=0; clk=0
end=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "${end}" ]; do
    b=$(cat "${DEV}/gpu_busy_percent")
    c=$(grep '\*' "${DEV}/pp_dpm_sclk" | sed -E 's/.*: ([0-9]+)Mhz.*/\1/')
    sum=$((sum + b)); clk=$((clk + ${c:-0})); n=$((n + 1))
    sleep 0.2
done
echo "gpu busy avg $((sum / n))% over ${n} samples, sclk avg $((clk / n)) MHz"
