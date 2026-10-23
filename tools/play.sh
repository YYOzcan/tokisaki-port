#!/usr/bin/env bash
# Tokisaki Port -- play in a window (Vulkan), driven from the keyboard.
#
#   Enter        START          Arrow keys   D-pad
#   Space        Cross          Esc / Bksp   Circle
#   R / F        Square         T / C        Triangle
#   Q / 1        L1             E / 3        R1
#   Tab / `      SELECT
#
# A connected game controller works as well (SDL).
# The window title shows the running build; KZ2_FPS=1 prints presented
# frames per second to the terminal.
#
# KZ2_GPU=igpu renders on the integrated GPU of a hybrid laptop. On the
# development machine (Radeon 680M + RTX 4050 Laptop) it is the faster one --
# the discrete GPU stays at low clocks under this load -- but the level start
# can stall there (docs/PROGRESS.md section 45); the default is the discrete GPU.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PS3RECOMP_VK_WINDOW=1
unset KZ2_RSX PAD_SCRIPT
export KZ2_FPS="${KZ2_FPS:-1}"
case "${KZ2_GPU:-}" in
    igpu|integrated) export PS3RECOMP_VK_PREFER=integrated ;;
esac
exec "${ROOT}/tools/run.sh" "$@"
