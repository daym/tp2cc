#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
EXAMPLES_DIR="$ROOT/examples"
OUT_DIR="${OUT_DIR:-$ROOT/build/examples}"
STATUS=0

mkdir -p "$OUT_DIR"

for src in "$EXAMPLES_DIR"/*.pas; do
  base=${src##*/}
  name=${base%.pas}
  expected="$EXAMPLES_DIR/$name.out"
  actual="$OUT_DIR/$name.actual"
  exe="$OUT_DIR/$name"

  if [ ! -f "$expected" ]; then
    echo "error: missing expected output file $expected" >&2
    exit 1
  fi

  echo "==> $name"
  "$ROOT/use-fpc.sh" "-o$exe" "$src"
  "$exe" >"$actual"

  if cmp -s "$expected" "$actual"; then
    echo "ok: $name"
    continue
  fi

  echo "mismatch: $name" >&2
  diff -u "$expected" "$actual" >&2 || true
  STATUS=1
done

exit "$STATUS"
