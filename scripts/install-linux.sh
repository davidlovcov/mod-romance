#!/usr/bin/env bash
# Builds AzerothCore (Playerbot fork) + mod-playerbots + mod-true-companion on Linux.
#   scripts/install-linux.sh [--src DIR] [--prefix DIR] [--jobs N] [--pinned]
# Run it from inside modules/mod-true-companion of an existing checkout, or pass --src for a fresh one.
# --pinned checks out the exact AzerothCore / mod-playerbots commits this version was built against.
set -euo pipefail

MOD="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$(basename "$(dirname "$MOD")")" = "modules" ]; then
  SRC="$(cd "$MOD/../.." && pwd)"
else
  SRC="$HOME/azerothcore"
fi
PREFIX="$HOME/azeroth-server"
JOBS="$(nproc 2>/dev/null || echo 2)"
PINNED=0
AC_COMMIT=93333d6a9eb00fdf0179adde30da3c17618207d6
PB_COMMIT=05a1f6fceb8a32aa879b13f5f48934c6e46a774c

while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC="$2"; shift ;;
    --prefix) PREFIX="$2"; shift ;;
    --jobs) JOBS="$2"; shift ;;
    --pinned) PINNED=1 ;;
    *) echo "unknown option $1"; exit 1 ;;
  esac
  shift
done

[ -d "$SRC" ] || git clone https://github.com/mod-playerbots/azerothcore-wotlk.git --branch=Playerbot "$SRC"
[ -d "$SRC/modules/mod-playerbots" ] || git clone https://github.com/mod-playerbots/mod-playerbots.git --branch=master "$SRC/modules/mod-playerbots"
if [ "$MOD" != "$SRC/modules/mod-true-companion" ] && [ ! -d "$SRC/modules/mod-true-companion" ]; then
  git clone https://github.com/davidlovcov/mod-true-companion.git "$SRC/modules/mod-true-companion"
fi
if [ "$PINNED" = 1 ]; then
  git -C "$SRC" fetch --depth 1 origin "$AC_COMMIT" && git -C "$SRC" checkout "$AC_COMMIT"
  git -C "$SRC/modules/mod-playerbots" fetch --depth 1 origin "$PB_COMMIT" && git -C "$SRC/modules/mod-playerbots" checkout "$PB_COMMIT"
fi

echo "== configuring $SRC"
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DSCRIPTS=static -DMODULES=static -DTOOLS_BUILD=all
echo "== building with $JOBS jobs (this takes a while)"
cmake --build "$SRC/build" -j"$JOBS"
cmake --install "$SRC/build"

CONF="$PREFIX/etc/modules"
for c in mod_true_companion playerbots; do
  if [ -f "$CONF/$c.conf.dist" ] && [ ! -f "$CONF/$c.conf" ]; then cp "$CONF/$c.conf.dist" "$CONF/$c.conf"; echo "created $CONF/$c.conf"; fi
done

if command -v ollama >/dev/null 2>&1; then
  ollama pull qwen3.5:4b || true
  ollama pull qwen3:4b-instruct || true
else
  echo "!! Ollama not found on this machine. Install it here or point TrueCompanion.Ollama.Url to the host running it."
fi
echo "Done. Start authserver and worldserver from $PREFIX/bin. Addon: https://github.com/davidlovcov/azerothcore-true-companion-addon"
