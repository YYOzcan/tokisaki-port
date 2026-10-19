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
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PS3RECOMP_VK_WINDOW=1
unset KZ2_RSX PAD_SCRIPT
export KZ2_FPS="${KZ2_FPS:-1}"
exec "${ROOT}/tools/run.sh" "$@"
