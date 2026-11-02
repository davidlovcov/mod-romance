#!/usr/bin/env bash
# Switches an AzerothCore server from mod-romance to mod-true-companion (the same module, renamed).
#
#   scripts/migrate-from-mod-romance.sh [AC_SOURCE_DIR] [CONFIG_DIR]
#
#   AC_SOURCE_DIR  your AzerothCore checkout (default: the one this module sits in, else ~/azerothcore-wotlk)
#   CONFIG_DIR     where worldserver.conf's module configs live (default: <AC_SOURCE_DIR>/env/dist/etc/modules)
#
# What it does (it asks before each step that changes something):
#   1. Moves modules/mod-romance out of the modules folder (both would be built and both would answer).
#   2. Writes mod_true_companion.conf from your mod_romance.conf: same values, keys renamed
#      Romance.* -> TrueCompanion.*. The old file is left untouched.
# Your data needs nothing: on the next worldserver start the DB updater renames the romance_* tables.
set -euo pipefail

MOD="$(cd "$(dirname "$0")/.." && pwd)"
if [ $# -ge 1 ]; then
  SRC="$(cd "$1" && pwd)"
elif [ "$(basename "$(dirname "$MOD")")" = "modules" ]; then
  SRC="$(cd "$MOD/../.." && pwd)"
else
  SRC="$HOME/azerothcore-wotlk"
fi
CONF_DIR="${2:-$SRC/env/dist/etc/modules}"

ask() { local a; read -r -p "$1 [y/N] " a; [ "$a" = "y" ] || [ "$a" = "Y" ]; }

echo "AzerothCore source: $SRC"
echo "Module configs:     $CONF_DIR"
[ -d "$SRC/modules" ] || { echo "No modules folder in $SRC - pass your AzerothCore checkout as the first argument."; exit 1; }

# 1. the old module
OLD="$SRC/modules/mod-romance"
if [ -d "$OLD" ]; then
  DEST="$SRC/mod-romance.old-$(date +%Y%m%d-%H%M%S)"
  if ask "Move $OLD to $DEST (out of the build)?"; then
    mv "$OLD" "$DEST"
    echo "  moved. Delete it once everything works."
  else
    echo "  left in place. Remove it before building, or both modules run."
  fi
else
  echo "modules/mod-romance: not there (fine)."
fi
[ -d "$SRC/modules/mod-true-companion" ] || echo "Note: modules/mod-true-companion is missing - clone it there before building."

# 2. the config
OLD_CONF="$CONF_DIR/mod_romance.conf"
NEW_CONF="$CONF_DIR/mod_true_companion.conf"
if [ ! -f "$OLD_CONF" ]; then
  echo "No $OLD_CONF - nothing to convert (the defaults from mod_true_companion.conf.dist are used)."
elif [ -f "$NEW_CONF" ]; then
  echo "$NEW_CONF already exists - not overwritten."
elif ask "Create $NEW_CONF from $OLD_CONF (keys renamed to TrueCompanion.*)?"; then
  sed -e 's/\bRomance\.\([A-Z]\)/TrueCompanion.\1/g' -e 's/AzerothRomance/TrueCompanion/g' \
      -e 's/mod-romance/mod-true-companion/g' -e 's/mod_romance/mod_true_companion/g' "$OLD_CONF" > "$NEW_CONF"
  echo "  written ($(grep -c '^TrueCompanion\.' "$NEW_CONF") settings). mod_romance.conf is no longer read; delete it when you like."
fi

cat <<EOF

Next:
  - Rebuild: re-run cmake in your build folder (the module's files were renamed), then build and install.
  - Start worldserver: the log shows "[mod-true-companion] v... enabled" and the tables are renamed.
  - Addon: delete Interface/AddOns/AzerothRomance and copy TrueCompanion there. To keep the addon's own
    settings (window position, your commands, ignored quests), rename
    WTF/Account/<ACCOUNT>/SavedVariables/AzerothRomance.lua to TrueCompanion.lua before logging in.
EOF
