#!/bin/sh
# End-to-end build: p2cc translator -> emit Pascal->C++ -> compile -> link pp.
#
# Steps:
#   1. make        -- build ./build/bin/p2cc (64-bit, host toolchain)
#   2. emit-all    -- translate pp.pas and its recursive `uses` tree to
#                     build/emitted/*.cc
#   3. compile     -- compile every emitted p_*.cc with 32-bit g++ from guix
#                     (fpc 0.99 targets i386; runtime assumes 32-bit pointers)
#   4. link        -- link build/emitted/pp from the reachable object files
#
# Uses guix shell --system=i686-linux for the 32-bit toolchain. Do NOT run
# with host g++ -- the runtime (p2cc_rt/prelude.h) is sized for 32-bit
# pointers and the link will succeed but every pointer-arithmetic site in
# the translated fpc source will corrupt memory.

set -eu

cd "$(dirname "$0")"
ROOT="$(pwd)"

JOBS="${JOBS:-8}"
GUIX_CXX='guix shell --system=i686-linux gcc-toolchain -- g++'
ENTRY_FILE="../rpm/compiler/pp.pas"
OUT_DIR="build/emitted"

echo "== [1/4] build p2cc translator =="
make -j"$JOBS"

echo "== [2/4] emit C++ from Pascal =="
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
./build/bin/p2cc emit-all "$ENTRY_FILE" "$OUT_DIR"

echo "== [3/4] compile emitted units (32-bit) =="
JOBS="$JOBS" CXX="$GUIX_CXX" tools/compile_emitted.sh "$OUT_DIR"

echo "== [4/4] link pp =="
cd "$OUT_DIR"
$GUIX_CXX -m32 -O0 -g p_*.o -o pp

ls -la pp
echo "ok: $ROOT/$OUT_DIR/pp"
