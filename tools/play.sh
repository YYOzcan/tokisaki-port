#!/usr/bin/env bash
# Tokisaki Port -- play in a window (Vulkan), with keyboard and mouse.
#
#   W A S D      move (left stick)     Mouse        look (right stick)
#   Left click   fire (R1)             Right click  aim / zoom (L1)
#   Space        jump (Cross)          C            crouch (Circle)
#   Ctrl (hold) + mouse: tilt the SIXAXIS (turn valves, arm charges)
#   R / F        reload (Square)       T / C        switch weapon (Triangle)
#   G            grenade (R2)          Z            cover (L2)
#   Left Shift   sprint (L3)           V / middle   melee (R3)
#   Q / E        L1 / R1               Arrow keys   D-pad
#   Enter        START (pause)         Tab / `      SELECT
#   Esc / Bksp   Circle (back in menus)
#   F1           release / recapture the mouse
#
# The mouse is captured while the window has the focus. PS3_PAD_MOUSE_SENS
# (default 3) sets the look speed; PS3_PAD_KBM=0 restores the old layout with
# WASD on the d-pad.
# A connected game controller works as well (SDL).
# The window title shows the running build; KZ2_FPS=1 prints presented
# frames per second to the terminal.
#
# GPU: on a hybrid laptop the integrated GPU is used by default -- on the
# development machine (Radeon 680M + RTX 4050 Laptop) it is the faster one,
# the discrete GPU stays at low clocks under this load. KZ2_GPU=dgpu forces
# the discrete GPU, KZ2_GPU=igpu the integrated one.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PS3RECOMP_VK_WINDOW=1
unset KZ2_RSX PAD_SCRIPT
export KZ2_FPS="${KZ2_FPS:-1}"
case "${KZ2_GPU:-}" in
    igpu|integrated) export PS3RECOMP_VK_PREFER=integrated ;;
    dgpu|discrete)   export PS3RECOMP_VK_PREFER=discrete ;;
esac
exec "${ROOT}/tools/run.sh" "$@"
