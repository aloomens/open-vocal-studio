#!/bin/sh
# Installs the Open Vocal Studio VST3 and LV2 plugins for the current user
# (~/.vst3 and ~/.lv2, where Linux hosts look by default). No root needed.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
name="Open Vocal Studio"

install_bundle() {   # $1 = source folder, $2 = destination folder
    if [ ! -d "$1" ]; then echo "Missing: $1" >&2; exit 1; fi
    mkdir -p "$2"
    rm -rf "$2/$(basename "$1")"
    cp -R "$1" "$2/"
    echo "Installed $(basename "$1") to $2"
}

install_bundle "$here/VST3/$name.vst3" "$HOME/.vst3"
install_bundle "$here/LV2/$name.lv2" "$HOME/.lv2"
chmod +x "$here/Standalone/$name" 2>/dev/null || true
echo "Done. Rescan plugins in your DAW."
