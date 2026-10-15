#!/usr/bin/env bash
# Tokisaki Port -- resolve logs/ftrace.txt (see src/kz2_ftrace.cpp) to names.
#   tools/ftrace_resolve.sh [binary=build-ftrace/killzone2]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-${ROOT}/build-ftrace/killzone2}"
nm --defined-only "${BIN}" | awk '{print $1, $3}' | sort > /tmp/kz2_nm.$$
sort -k2 "${ROOT}/logs/ftrace.txt" | while read -r n off; do
    printf "%s %016x\n" "$n" "$((16#$off))"
done | sort -k2 | join -1 2 -2 1 - /tmp/kz2_nm.$$ | awk '{print $2, $3}' | sort -rn
rm -f /tmp/kz2_nm.$$
