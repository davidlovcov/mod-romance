#!/usr/bin/env bash
# Runs the automated tests of mod-true-companion (no AzerothCore build needed).
#   scripts/run-tests.sh [--sql] [--sanitize]
#   ADDON_DIR=../azerothcore-true-companion-addon scripts/run-tests.sh   # also runs the C++ <-> Lua wire interop test
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build-tests}"
SQL=0
SAN=OFF
for a in "$@"; do
  case "$a" in
    --sql) SQL=1 ;;
    --sanitize) SAN=ON; BUILD="$BUILD-asan" ;;
    *) echo "unknown option $a"; exit 1 ;;
  esac
done

echo "== C++ tests"
cmake -S "$ROOT/tests" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug -DTRUE_COMPANION_SANITIZE=$SAN >/dev/null
cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 2)"
"$BUILD/true_companion_tests"

if [ -n "${ADDON_DIR:-}" ]; then
  echo "== C++ <-> Lua wire interop (addon at $ADDON_DIR)"
  (cd "$ADDON_DIR" && lua5.1 tests/test_interop.lua TrueCompanion "$BUILD/true_companion_wire")
fi

if [ "$SQL" = 1 ]; then
  echo "== SQL against local MySQL/MariaDB"
  python3 "$ROOT/tests/test_sql.py" "$ROOT" "${MYSQL_CMD:-sudo mysql}"
fi
echo "All selected test suites passed."
