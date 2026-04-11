#!/usr/bin/env bash
# Portable launcher - resolves the bundle directory so that RetroArch
# finds its bundled assets, cores, databases, and shared libraries
# regardless of the user's current working directory.
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$SCRIPT_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBRETRO_ASSETS_DIRECTORY="$SCRIPT_DIR/assets"
cd "$SCRIPT_DIR"
exec "$SCRIPT_DIR/retroarch" "$@"

