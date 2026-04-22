#!/bin/sh
#
# Scouting harness for translating fpc-2.0.0's compiler with tp2cc.
#
# fpc-2.0.0 uses Delphi-style classes, AnsiString, try/except, and
# properties that tp2cc doesn't fully implement yet.  This script
# just runs the emit stage so we can see the class of errors tp2cc
# hits first and prioritise features to add.  It intentionally does
# NOT attempt to compile or link the emitted C++ -- we're only
# measuring parse/emit coverage.
#
# Point FPC200_SRC at a cloned fpc git tag:
#   git clone --depth 1 --branch release_2_0_0 \
#     https://gitlab.com/freepascal.org/fpc/source.git fpc-2.0.0
#   FPC200_SRC=$PWD/fpc-2.0.0 ./bootstrap-fpc-2.sh
#
# Or set it to an already-extracted tree.  No --system or guix shell
# needed because we don't run the output.

set -eu

cd "$(dirname "$0")"
ROOT=$(pwd)

SOURCE_DIR="${FPC200_SRC:-$ROOT/../fpc-2.0.0/src}"
SCOUT_DIR="${SCOUT_DIR:-$ROOT/build/fpc-2.0.0-scout}"
ENTRY_FILE="$SOURCE_DIR/compiler/pp.pas"

if [ ! -f "$ENTRY_FILE" ]; then
  echo "error: set FPC200_SRC to an fpc-2.0.0 source tree containing compiler/pp.pas" >&2
  echo "current: $ENTRY_FILE" >&2
  exit 1
fi

JOBS="${JOBS:-8}"

echo "== build tp2cc translator =="
make -j"$JOBS" build/bin/tp2cc

echo "== emit-all of fpc-2.0.0 compiler (scouting only) =="
rm -rf "$SCOUT_DIR"
mkdir -p "$SCOUT_DIR"

# Feed tp2cc the defines and -Fu/-Fi paths that match a hypothetical
# linux/i386 native build.  fpc-2.0.0 uses `CPUI386' rather than the
# `I386' symbol 1.0.6 checked for, plus a dozen NOTARGET* opt-outs,
# so the define set is intentionally wider than bootstrap-fpc-1.sh's.
LOG="$SCOUT_DIR/emit.log"
./build/bin/tp2cc emit-all \
  -dFPC -dCPUI386 -dI386 -dLINUX -dUNIX \
  -dNOTARGETAMIGA -dNOTARGETBEOS -dNOTARGETFREEBSD \
  -dNOTARGETGO32V1 -dNOTARGETGO32V2 -dNOTARGETOS2 \
  -dNOTARGETPALMOS -dNOTARGETQNX -dNOTARGETSUNOS \
  -dNOTARGETWIN32 -dNOTARGETNETBSD -dNOTARGETOPENBSD \
  -dNOTARGETDARWIN -dNOTARGETNETWARE -dNOTARGETEMX \
  -Fu"$SOURCE_DIR/compiler" \
  -Fu"$SOURCE_DIR/compiler/i386" \
  -Fu"$SOURCE_DIR/compiler/systems" \
  -Fi"$SOURCE_DIR/compiler" \
  -Fi"$SOURCE_DIR/compiler/i386" \
  -Fi"$SOURCE_DIR/rtl/inc" \
  -Fi"$SOURCE_DIR/rtl/i386" \
  -Fi"$SOURCE_DIR/rtl/linux" \
  "$ENTRY_FILE" "$SCOUT_DIR" 2>&1 | tee "$LOG" || true

echo
echo "== scout summary =="
grep -cE '^[^:]+:[0-9]+:[0-9]+: error:' "$LOG" | head -1 | \
  awk '{print "parse/emit errors: " $1}'
echo
echo "top-5 error kinds:"
grep -oE "error: [^'{]*" "$LOG" | sed 's/^error: //' | sort | uniq -c | \
  sort -rn | head -5
echo
echo "files with most errors:"
grep -oE '^[^:]+\.pas' "$LOG" | sort | uniq -c | sort -rn | head -5
echo
echo "log: $LOG"
