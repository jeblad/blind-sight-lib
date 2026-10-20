#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Dev-time convenience: installs a .desktop launcher entry and the bat
# icon (resource/icons/blind-sight-bat-app-icon.svg — the wordmark bat on
# a mid-gray circle, so it reads on both light and dark GNOME themes) for
# the current user, so the demo shows up properly in the GNOME dash/
# taskbar/app grid instead of a
# generic icon (John, 2026-09-21: "vår bat bør bli applikasjonsikon" —
# picking up the ".desktop file + dash/taskbar icon" item explicitly
# deferred earlier in TODO.md).
#
# Not a real packaging/install step — this project has no CMake
# install() target yet (see demo/CMakeLists.txt's own comments on why),
# so this points straight at wherever the dev build actually is, via
# data/io.github.jeblad.blind-sight-demo.desktop.in's @EXEC_PATH@
# placeholder, rather than some canonical installed location. Re-run any
# time the build moves (a different preset, a fresh checkout elsewhere)
# to refresh it.
#
# Installs into the user's own ~/.local/share/ — never system-wide, and
# never run automatically by anything else in this repo; this is opt-in,
# by hand.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_ID="io.github.jeblad.blind-sight-demo"
EXEC_PATH="$REPO_ROOT/build/default/demo/blind-sight-demo"

if [ ! -x "$EXEC_PATH" ]; then
    echo "error: $EXEC_PATH doesn't exist or isn't executable." >&2
    echo "       Build the default preset first:" >&2
    echo "         cmake --build --preset default" >&2
    exit 1
fi

DESKTOP_DIR="$HOME/.local/share/applications"
ICON_DIR="$HOME/.local/share/icons/hicolor/scalable/apps"
mkdir -p "$DESKTOP_DIR" "$ICON_DIR"

sed "s|@EXEC_PATH@|$EXEC_PATH|" \
    "$SCRIPT_DIR/data/$APP_ID.desktop.in" \
    > "$DESKTOP_DIR/$APP_ID.desktop"
cp "$REPO_ROOT/resource/icons/blind-sight-bat-app-icon.svg" "$ICON_DIR/$APP_ID.svg"

# Neither is required for the entry to work eventually (GNOME Shell
# rescans on its own), but both make it show up immediately rather than
# after the next cache refresh.
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$DESKTOP_DIR"
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
fi

echo "Installed:"
echo "  $DESKTOP_DIR/$APP_ID.desktop"
echo "  $ICON_DIR/$APP_ID.svg"
echo "Pointing at: $EXEC_PATH"
