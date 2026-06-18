#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT="$SCRIPT_DIR"
OUT_DIR="$ROOT/build/emitted"
PP="${PP:-$OUT_DIR/pp}"
CFG_DIR="${PPC_CONFIG_PATH:-$OUT_DIR}"
if [ -f "$CFG_DIR/fpc.cfg" ]; then
  CFG="$CFG_DIR/fpc.cfg"
else
  CFG="$CFG_DIR/ppc386.cfg"
fi
FPCDIR_DEFAULT="$ROOT/../rpm/"
AS="${AS:-as}"
LD="${LD:-ld}"
if [ "${STARTUP_AS+x}" != x ]; then
  STARTUP_AS="$ROOT/../rpm/rtl/linux/i386/prt0.as"
fi
if [ "${STARTUP_ASFLAGS+x}" != x ]; then
  case "$STARTUP_AS" in
    */x86_64/*) STARTUP_ASFLAGS="--64" ;;
    *) STARTUP_ASFLAGS="--32" ;;
  esac
fi
KEEP_WORK="${KEEP_TP2CC_WORK:-0}"
FORCE_BUILD_FLAG="-B"
if [ "${USE_FPC_FORCE_BUILD:-1}" = "0" ]; then
  FORCE_BUILD_FLAG=
fi

if [ ! -x "$PP" ]; then
  echo "error: $PP not found; run the relevant bootstrap script first" >&2
  exit 1
fi

if [ ! -f "$CFG" ]; then
  echo "error: neither $CFG_DIR/fpc.cfg nor $CFG_DIR/ppc386.cfg was found; run the relevant bootstrap script first" >&2
  exit 1
fi

if [ -n "$STARTUP_AS" ] && [ ! -f "$STARTUP_AS" ]; then
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

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/tp2cc-use-fpc.XXXXXX")
build_dir="$work_dir/out"
startup_obj="$build_dir/prt0.o"
pp_stdout="$work_dir/pp.stdout"
pp_script="$build_dir/ppas.sh"
link_res="$build_dir/link.res"
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
  "$PP" "$@" $FORCE_BUILD_FLAG -a -s "-FE$build_dir" "-FU$build_dir" >"$pp_stdout"
then
  cat "$pp_stdout" >&2
  exit 1
fi

if grep -q '^#!/bin/bash' "$pp_stdout"; then
  sed '/^#!\/bin\/bash$/,$d' "$pp_stdout" >"$link_res"
  sed -n '/^#!\/bin\/bash$/,$p' "$pp_stdout" >"$pp_script"
fi

if [ ! -s "$pp_script" ]; then
  echo "error: compiler succeeded but produced no ppas.sh" >&2
  cat "$pp_stdout" >&2
  exit 1
fi

chmod +x "$pp_script"

if [ -n "$STARTUP_AS" ]; then
  "$AS" $STARTUP_ASFLAGS -o "$startup_obj" "$STARTUP_AS"
fi

sh "$pp_script"

produced_output=""

case "$output_name" in
  /*)
    if [ -x "$output_path" ]; then
      produced_output="$output_path"
    fi
    ;;
esac

if [ -z "$produced_output" ]; then
  produced_output="$build_dir/$output_name"
fi

if [ ! -x "$produced_output" ]; then
  produced_output="$build_dir/${input_file##*/}"
  case "$produced_output" in
    *.*) produced_output=${produced_output%.*} ;;
  esac
fi

if [ ! -x "$produced_output" ]; then
  echo "error: compiler driver finished but produced no executable" >&2
  cat "$pp_stdout" >&2
  exit 1
fi

if [ "$produced_output" = "$output_path" ]; then
  exit 0
fi

mkdir -p "$(dirname "$output_path")"
cp -f "$produced_output" "$output_path"
