#!/usr/bin/env bash
# Tokisaki Port -- add "Killzone 2" to the application menu (and the desktop,
# when there is one). It starts tools/play.sh in a window; the game's output
# goes to logs/play.log. With Tk installed, tools/launcher.py offers more
# (campaign quick start, GPU, mouse speed): KZ2_ENTRY=launcher picks it.
#   tools/install_desktop_entry.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPS="${XDG_DATA_HOME:-${HOME}/.local/share}/applications"
ENTRY="${APPS}/killzone2-tokisaki.desktop"
if [ "${KZ2_ENTRY:-play}" = launcher ]; then
    EXEC="exec python3 \\\"${ROOT}/tools/launcher.py\\\""
else
    EXEC="exec \\\"${ROOT}/tools/play.sh\\\" > \\\"${ROOT}/logs/play.log\\\" 2>&1"
fi
mkdir -p "${APPS}" "${ROOT}/logs"
cat > "${ENTRY}" <<EOF
[Desktop Entry]
Type=Application
Name=Killzone 2
GenericName=Killzone 2 (Tokisaki Port)
Comment=Statically recompiled Killzone 2 -- keyboard + mouse or a controller
Exec=sh -c "${EXEC}"
Path=${ROOT}
Icon=${ROOT}/game_dump/PS3_GAME/ICON0.PNG
Terminal=false
Categories=Game;
EOF
command -v desktop-file-validate > /dev/null && desktop-file-validate "${ENTRY}"
DESK="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
if [ -n "${DESK}" ] && [ -d "${DESK}" ] && [ "${DESK}" != "${HOME}" ]; then
    cp "${ENTRY}" "${DESK}/" && chmod +x "${DESK}/$(basename "${ENTRY}")"
fi
echo "${ENTRY}"
