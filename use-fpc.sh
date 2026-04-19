#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT="$SCRIPT_DIR"
OUT_DIR="$ROOT/build/emitted"
PP="${PP:-$OUT_DIR/pp}"
CFG_DIR="${PPC_CONFIG_PATH:-$OUT_DIR}"
CFG="$CFG_DIR/ppc386.cfg"
FPCDIR_DEFAULT="$ROOT/../rpm/"
AS="${AS:-as}"
LD="${LD:-ld}"
STARTUP_AS="${STARTUP_AS:-$ROOT/../rpm/rtl/linux/i386/prt0.as}"
KEEP_WORK="${KEEP_P2CC_WORK:-0}"

if [ ! -x "$PP" ]; then
  echo "error: $PP not found; run ./build-fpc.sh first" >&2
  exit 1
fi

if [ ! -f "$CFG" ]; then
  echo "error: $CFG not found; run ./build-fpc.sh first" >&2
  exit 1
fi

if [ ! -f "$STARTUP_AS" ]; then
  echo "error: startup file not found: $STARTUP_AS" >&2
  exit 1
fi

input_file=""
output_name=""

for arg in "$@"; do
  case "$arg" in
    -FE*|-FU*)
      echo "error: use-fpc.sh manages -FE/-FU internally; use -o for the final executable path" >&2
      exit 1
      ;;
    -o?*)
      output_name=${arg#-o}
      ;;
    -*)
      ;;
    *)
      input_file=$arg
      ;;
  esac
done

if [ -z "$input_file" ]; then
  PPC_CONFIG_PATH="$CFG_DIR" \
  FPCDIR="${FPCDIR:-$FPCDIR_DEFAULT}" \
  exec "$PP" "$@"
fi

case "$output_name" in
  "")
    output_name=${input_file##*/}
    case "$output_name" in
      *.*) output_name=${output_name%.*} ;;
    esac
    ;;
esac

case "$output_name" in
  /*) output_path=$output_name ;;
  *) output_path=$(pwd)/$output_name ;;
esac

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/p2cc-use-fpc.XXXXXX")
build_dir="$work_dir/out"
startup_obj="$work_dir/prt0.o"
pp_stdout="$work_dir/pp.stdout"
mkdir -p "$build_dir"

cleanup() {
  if [ "$KEEP_WORK" = "1" ] || [ "$KEEP_WORK" = "yes" ] || [ "$KEEP_WORK" = "true" ]; then
    echo "kept build artifacts in $work_dir" >&2
    return
  fi
  rm -rf "$work_dir"
}
trap cleanup EXIT INT TERM HUP

if ! PPC_CONFIG_PATH="$CFG_DIR" \
  FPCDIR="${FPCDIR:-$FPCDIR_DEFAULT}" \
  "$PP" "$@" -B -a -s "-FE$build_dir" "-FU$build_dir" >"$pp_stdout"
then
  cat "$pp_stdout" >&2
  exit 1
fi

set -- "$build_dir"/*.s
if [ "$1" = "$build_dir/*.s" ]; then
  echo "error: compiler succeeded but emitted no assembler files in $build_dir" >&2
  exit 1
fi

"$AS" --32 -o "$startup_obj" "$STARTUP_AS"

for asm_src do
  obj=${asm_src%.s}.o
  "$AS" --32 -o "$obj" "$asm_src"
done

set -- "$build_dir"/*.o
if [ "$1" = "$build_dir/*.o" ]; then
  echo "error: assembly succeeded but produced no object files in $build_dir" >&2
  exit 1
fi

mkdir -p "$(dirname "$output_path")"
"$LD" -m elf_i386 -o "$output_path" "$startup_obj" "$@"
