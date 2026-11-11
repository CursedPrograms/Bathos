#!/usr/bin/env bash
# Installs Bathos for this user on Linux: ~/.local/share/Bathos, ~/.local/bin/bathos and bathos-cli, a menu entry.
set -e
cd "$(dirname "$0")"
DEST="$HOME/.local/share/Bathos"
mkdir -p "$DEST" "$HOME/.local/bin" "$HOME/.local/share/applications"
cp -r build/Bathos build/bathos-cli build/libonnxruntime.so* build/models build/assets "$DEST/"
printf '#!/bin/sh\nLD_LIBRARY_PATH="%s" exec "%s/Bathos" "$@"\n' "$DEST" "$DEST" > "$HOME/.local/bin/bathos"
printf '#!/bin/sh\nLD_LIBRARY_PATH="%s" exec "%s/bathos-cli" "$@"\n' "$DEST" "$DEST" > "$HOME/.local/bin/bathos-cli"
chmod +x "$HOME/.local/bin/bathos" "$HOME/.local/bin/bathos-cli"
printf '[Desktop Entry]\nName=Bathos\nComment=A picture in, its depth map out\nExec=%s/.local/bin/bathos %%f\nIcon=%s/assets/logo.png\nType=Application\nCategories=Graphics;\n' "$HOME" "$DEST" > "$HOME/.local/share/applications/bathos.desktop"
echo "Installed: $DEST (run: bathos)"
