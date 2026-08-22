#!/bin/bash

set -e

PREFIX="${PREFIX:-$HOME/.local}"

BIN_FILE="$PREFIX/bin/gammactrl"
DESKTOP_FILE="$PREFIX/share/applications/org.gammactrl.desktop"
ICON_FILE="$PREFIX/share/icons/hicolor/scalable/apps/org.gammactrl.svg"

echo "Uninstalling gammactrl from $PREFIX ..."

remove_if_exists() {
    local f="$1"
    if [ -e "$f" ]; then
        rm -f "$f"
        echo "  removed: $f"
    else
        echo "  skipped (not found): $f"
    fi
}

remove_if_exists "$BIN_FILE"
remove_if_exists "$DESKTOP_FILE"
remove_if_exists "$ICON_FILE"

# Refresh the icon cache / desktop database if the tools are available,
# so the removed launcher/icon disappear from menus immediately.
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
fi

# Optional: clean up the local build directory created by build.sh/install.sh.
if [ -d "./build" ]; then
    read -p "Remove local build/ directory too? [y/N] " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf ./build
        echo "  removed: ./build"
    fi
fi

echo "gammactrl uninstalled."
