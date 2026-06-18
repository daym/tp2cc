#!/bin/sh
#
# Bootstrap FPC 3 from the checked-out source tree using tp2cc.
#
# Stages:
#   1. Copy a private clean source tree under build/.
#   2. Build `tp2cc`.
#   3. Translate `compiler/pp.pas` and its recursive `uses` tree to C++.
#   4. Compile and link the translated stage1 compiler.
#   5. Use stage1 `pp` to rebuild `compiler/pp.pas` as stage2.
#   6. Use stage2 `pp` to rebuild `compiler/pp.pas` again as stage3.

set -eu

cd "$(dirname "$0")"
ROOT=$(pwd)

JOBS="${JOBS:-8}"
CXX="${CXX:-g++}"
CC="${CC:-gcc}"
SOURCE_DIR="${FPC3_SRC:-$ROOT/../fpc-3.0.0}"

# Print the source path AND its declared version up front so a silent
# target mismatch can't hide.
fpc_source_version() {
  local v="$SOURCE_DIR/compiler/version.pas"
  [ -r "$v" ] || { echo "unknown"; return; }
  awk -F"'" '
    /version_nr *=/ { vn=$2 }
    /release_nr *=/ { rn=$2 }
    /patch_nr *=/   { pn=$2 }
    END { if (vn != "") print vn"."rn"."pn; else print "unknown" }
  ' "$v"
}

fpc_compiler_version_defines() {
  # These are the symbols of the compiler compiling the source, not the
  # version declared by the source tree being translated.
  printf '%s ' \
    -dFPC_VERSION:=2 -dFPC_RELEASE:=6 -dFPC_PATCH:=0 \
    -dFPC_FULLVERSION:=20600 \
    -dVER2 -dVER2_6 -dVER2_6_0
}

# Pick the stage1 target CPU.  Override with TARGET_CPU=...; otherwise read
# `$CXX -dumpmachine` so the translated compiler matches the g++ that will
# link it.  Supported values: i386, x86_64.
detect_target_cpu() {
  machine="$($CXX -dumpmachine 2>/dev/null || true)"
  case "$machine" in
    i?86-*) echo i386 ;;
    x86_64-*|amd64-*) echo x86_64 ;;
    *)
      echo "error: cannot infer TARGET_CPU from '$machine' (expected i?86-* or x86_64-*; set TARGET_CPU explicitly)" >&2
      exit 1
      ;;
  esac
}
TARGET_CPU="${TARGET_CPU:-$(detect_target_cpu)}"
case "$TARGET_CPU" in
  i386|x86_64) ;;
  *)
    echo "error: TARGET_CPU='$TARGET_CPU' not supported (use i386 or x86_64)" >&2
    exit 1
    ;;
esac
echo "stage1 target CPU: $TARGET_CPU"

# Stage1 conditional-compile defines shared by every CPU target.  These are
# source-language symbols that FPC's option parser would normally define after
# it starts; tp2cc must provide them before parsing because it evaluates
# {$ifdef} while translating the compiler.
fpc_stage1_shared_defines() {
  printf '%s ' \
    -dFPC \
    $(fpc_compiler_version_defines) \
    -dEXTERN_MSG \
    -dLINUX -dUNIX -dHASUNIX \
    -dFPC_HAS_OPERATOR_ENUMERATOR -dFPC_HAS_CONSTREF \
    -dFPC_STATICRIPFIXED \
    -dFPC_HAS_UNICODESTRING -dFPC_RTTI_PACKSET1 \
    -dFPC_HAS_CEXTENDED -dFPC_HAS_INTERNAL_ABS_LONG \
    -dFPC_HAS_INTERNAL_SAR -dFPC_HAS_MEMBAR -dFPC_SETBASE_USED \
    -dSTR_CONCAT_PROCS -dFPC_HAS_FEATURE_SUPPORT \
    -dFPC_HAS_TYPE_DOUBLE -dFPC_HAS_TYPE_SINGLE \
    -dFPC_LINK_STATIC \
    -dFPC_HAS_FEATURE_HEAP -dFPC_HAS_FEATURE_INITFINAL \
    -dFPC_HAS_FEATURE_RTTI -dFPC_HAS_FEATURE_CLASSES \
    -dFPC_HAS_FEATURE_EXCEPTIONS -dFPC_HAS_FEATURE_EXITCODE \
    -dFPC_HAS_FEATURE_ANSISTRINGS -dFPC_HAS_FEATURE_WIDESTRINGS \
    -dFPC_HAS_FEATURE_TEXTIO -dFPC_HAS_FEATURE_CONSOLEIO \
    -dFPC_HAS_FEATURE_FILEIO -dFPC_HAS_FEATURE_RANDOM \
    -dFPC_HAS_FEATURE_VARIANTS -dFPC_HAS_FEATURE_OBJECTS \
    -dFPC_HAS_FEATURE_DYNARRAYS -dFPC_HAS_FEATURE_THREADING \
    -dFPC_HAS_FEATURE_COMMANDARGS -dFPC_HAS_FEATURE_PROCESSES \
    -dFPC_HAS_FEATURE_STACKCHECK -dFPC_HAS_FEATURE_DYNLIBS \
    -dFPC_HAS_FEATURE_RESOURCES \
    -dFPC_WIDESTRING_EQUAL_UNICODESTRING \
    -dFPC_VARIANTCOPY_FIXED -dFPC_DYNARRAYCOPY_FIXED \
    -dFPC_HAS_RESSTRINITS -dFPC_HAS_FEATURE_UNICODESTRINGS
}

fpc_stage1_source_version_defines() {
  case "$(fpc_source_version)" in
    3.0.*)
      # FPC 3.0.x compiler/options.pas defines these system macros at runtime,
      # and 3.0.x compiler/RTL sources still use them in conditional
      # compilation.  tp2cc evaluates those conditionals before the translated
      # compiler can run options.pas, so the bootstrap must provide the same
      # source-version-specific feature symbols up front.  FPC 3.2.0 no longer
      # consumes them, so do not leak them into the 3.2 branch.
      printf '%s ' \
        -dRESSTRSECTIONS \
        -dFPC_HASFIXED64BITVARIANT \
        -dFPC_HASINTERNALOLEVARIANT2VARIANTCAST \
        -dFPC_HAS_VARSETS \
        -dFPC_HAS_VALGRINDBOOL \
        -dFPC_HAS_STR_CURRENCY \
        -dFPC_REAL2REAL_FIXED \
        -dFPC_STRTOCHARARRAYPROC \
        -dFPC_STRTOSHORTSTRINGPROC \
        -dFPC_OBJFPC_EXTENDED_IF
      ;;
    3.2.*)
      ;;
  esac
}

# CPU-specific stage1 defines + extra per-CPU helper symbols.  Values that are
# Pascal constants in FPC sources, such as first_mm_imreg, are repeated here as
# textual preprocessor inputs because {$if ...} cannot see unit constants.
fpc_stage1_cpu_defines() {
  case "$TARGET_CPU" in
    i386)
      printf '%s ' \
        -dI386 -dCPU86 -dCPU87 -dCPU386 \
        -dCPUI386 -dCPU32 -dCPUX86 \
        -dFPC_HAS_TYPE_EXTENDED \
        -dINTERNAL_BACKTRACE -dREGCALL \
        -dFPC_HAS_INTERNAL_ABS_INT64 \
        -dFPC_HAS_INTERNAL_BSF -dFPC_HAS_INTERNAL_BSR \
        -dfirst_mm_imreg:=8 \
        -dNOAG386NSM -d__NOWINPECOFF__ \
        -dENDIAN_LITTLE -dFPC_LITTLE_ENDIAN \
        -dFPC_HAS_INTERNAL_ROX -dFPC_HAS_INTERNAL_ABS_LONG
      ;;
    x86_64)
      printf '%s ' \
        -dx86_64 -dCPUX86_64 -dCPUAMD64 -dCPU64 -dCPUX64 \
        -dFPC_HAS_TYPE_EXTENDED \
        -dINTERNAL_BACKTRACE -dREGCALL \
        -dFPC_HAS_INTERNAL_ABS_INT64 -dFPC_HAS_RIP_RELATIVE \
        -dFPC_HAS_INTERNAL_BSF -dFPC_HAS_INTERNAL_BSR \
        -dfirst_mm_imreg:=16 \
        -dNOAGX86_64NSM \
        -dENDIAN_LITTLE -dFPC_LITTLE_ENDIAN \
        -dFPC_HAS_INTERNAL_ROX -dFPC_HAS_INTERNAL_ABS_LONG
      ;;
  esac
}

fpc_target_prune_win_defines() {
  case "$(fpc_source_version)" in
    3.*)
      # NOTARGETWIN removes Win/COFF target units such as ogcoff from the
      # compiler target registry, and is read by shared x86 code in
      # compiler/x86/cgx86.pas.  NOTARGETWINCE is the same gate for the
      # ARM WinCE backend in compiler/arm/cputarg.pas.  i386 also needs
      # __NOWINPECOFF__ (consumed only by compiler/i386/cgcpu.pas); that
      # symbol is emitted via fpc_stage1_cpu_defines for i386, not here.
      printf '%s' "-dNOTARGETWIN -dNOTARGETWINCE"
      ;;
    *)
      echo "error: bootstrap-fpc-3.sh has no Win target-prune rule for FPC $(fpc_source_version)" >&2
      exit 1
      ;;
  esac
}

case "$(fpc_source_version)" in
  3.*)
    ;;
  *)
    echo "error: FPC3_SRC must point at an FPC 3.x source tree, got version $(fpc_source_version) from $SOURCE_DIR" >&2
    exit 1
    ;;
esac
echo "FPC source: $SOURCE_DIR (version $(fpc_source_version))"

# Keep the translated compiler under sanitizer instrumentation.
# Default to UBSan-only for stage1 (recoverable) so the bootstrap can report
# multiple issues in one pass. Set SAN explicitly to include ASan if you need it.
SAN="${SAN:--fsanitize=undefined -fsanitize-recover=undefined -fno-omit-frame-pointer}"
CXXFLAGS="-std=gnu++20 -I. -O3 -g -pipe -Wno-narrowing -Wno-invalid-offsetof $SAN"
CFLAGS="-std=gnu11 -O3 -g -pipe $SAN"
export CXXFLAGS
export CFLAGS

# FPC frees most global state only at process exit. UBSan is recoverable so one
# translated compiler invocation can report every visible issue; the logged
# output is checked after those invocations so any UBSan report still fails the
# bootstrap.
if printf '%s' "$SAN" | tr ',' ' ' | grep -qw "address"; then
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:abort_on_error=1:print_stacktrace=1}"
else
  export ASAN_OPTIONS="${ASAN_OPTIONS-}"
fi
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=0:print_stacktrace=1}"

BOOT_ROOT="${BOOTSTRAP_ROOT:-$ROOT/build/fpc-3-bootstrap}"
mkdir -p "$BOOT_ROOT"
BOOT_ROOT=$(cd "$BOOT_ROOT" && pwd)
HOST_BUILD="${TP2CC_BUILD:-$ROOT/build-tp2cc-host}"
CLEAN_SRC="$BOOT_ROOT/source"
STAGE1_DIR="$BOOT_ROOT/stage1"
STAGE2_DIR="$BOOT_ROOT/stage2"
STAGE3_DIR="$BOOT_ROOT/stage3"
STAGE1_LOGDIR="$BOOT_ROOT/compile-logs-stage1"
SAN_LOGDIR="$BOOT_ROOT/sanitizer-logs"
ENTRY_FILE="$SOURCE_DIR/compiler/pp.pas"
MSG_FILE="$SOURCE_DIR/compiler/msg/errore.msg"

startup_asm_path() {
  case "$(fpc_source_version):$TARGET_CPU" in
    3.2.*:i386)
      # FPC 3.2 i386-linux moved startup code from external loader files into
      # sysinit units: si_prc.pp, si_c.pp, si_c21.pp, si_dll.pp, and si_uc.pp
      # include rtl/linux/i386/si_*.inc files that define the relevant `_start`
      # entry points. rtl/linux/Makefile.fpc therefore sets LOADERS= for
      # ARCH=i386, leaving no external prt0.as/cprt0.as to assemble here.
      ;;
    *)
      printf '%s\n' "$CLEAN_SRC/rtl/linux/$TARGET_CPU/prt0.as"
      ;;
  esac
}

startup_asflags() {
  case "$TARGET_CPU" in
    i386) printf '%s\n' "--32" ;;
    x86_64) printf '%s\n' "--64" ;;
  esac
}

STARTUP_AS="$(startup_asm_path)"
STARTUP_ASFLAGS="$(startup_asflags)"
RUNTIME_SHIM="$ROOT/include/tp2cc_rt/fenv_shim.c"

compiler_dir_flags() {
  printf '%s ' "-Fu$CLEAN_SRC/compiler" "-Fi$CLEAN_SRC/compiler"
  printf '%s ' "-Fu$CLEAN_SRC/compiler/$TARGET_CPU" "-Fi$CLEAN_SRC/compiler/$TARGET_CPU"
  printf '%s ' "-Fu$CLEAN_SRC/compiler/x86" "-Fi$CLEAN_SRC/compiler/x86"
  printf '%s ' "-Fu$CLEAN_SRC/compiler/systems" "-Fi$CLEAN_SRC/compiler/systems"
}

build_translator() {
  echo "== [1/7] build tp2cc translator =="
  # Keep the host translator build separate from the bootstrap output tree.
  # That avoids mixed-architecture object reuse and does not require wiping
  # the entire `build/` directory just to rebuild `tp2cc` for i686.
  rm -rf "$HOST_BUILD"
  make -j"$JOBS" BUILD="$HOST_BUILD" "$HOST_BUILD/bin/tp2cc"
}

copy_clean_source() {
  echo "== [2/7] copy clean FPC 3 source tree =="
  rm -rf "$CLEAN_SRC"
  mkdir -p "$BOOT_ROOT"
  cp -a "$SOURCE_DIR" "$CLEAN_SRC"
  find "$CLEAN_SRC" \( -name "*.ppu" -o -name "*.o" -o -name "*.s" -o -name "*.a" \) \
    -type f -delete
}

write_bootstrap_cfg() {
  # FPC looks for `fpc.cfg` under PPC_CONFIG_PATH; `ppc386.cfg` is only the
  # legacy fallback name.
  case "$TARGET_CPU" in
    i386)    cpu_define="-dI386" ;;
    x86_64)  cpu_define="-dx86_64" ;;
  esac
  cat > "$STAGE1_DIR/fpc.cfg" <<EOF
; generated by bootstrap-fpc-3.sh for translated FPC 3 (TARGET_CPU=$TARGET_CPU)
; Exclude dbgdwarf for the current bootstrap. This target's default debug
; format is still stabs, so dbgdwarf is not needed to build the compiler, and
; the dbgdwarf unit still uses array of const, which tp2cc does not yet
; support. Keep the define active from the start because cputarg.pas
; conditionally uses dbgdwarf when NoDbgDwarf is absent.
$cpu_define
-dNoDbgDwarf
-dEXTERN_MSG
-Sg
-Fi$CLEAN_SRC/rtl/inc
-Fi$CLEAN_SRC/rtl/$TARGET_CPU
-Fi$CLEAN_SRC/rtl/linux
-Fi$CLEAN_SRC/rtl/linux/$TARGET_CPU
-Fi$CLEAN_SRC/rtl/unix
-Fi$CLEAN_SRC/rtl/objpas
-Fi$CLEAN_SRC/rtl/objpas/sysutils
-Fu$CLEAN_SRC/rtl/inc
-Fu$CLEAN_SRC/rtl/linux
-Fu$CLEAN_SRC/rtl/$TARGET_CPU
-Fu$CLEAN_SRC/rtl/linux/$TARGET_CPU
-Fu$CLEAN_SRC/rtl/unix
-Fu$CLEAN_SRC/rtl/objpas
EOF
}

install_support_files() {
  stage_dir="$1"
  cp "$MSG_FILE" "$stage_dir/errore.msg"
  if [ "$stage_dir" != "$STAGE1_DIR" ]; then
    cp "$STAGE1_DIR/fpc.cfg" "$stage_dir/fpc.cfg"
  fi
}

fail_on_ubsan_reports() {
  log_file="$1"
  if grep -q ': runtime error:' "$log_file"; then
    echo "error: UBSan reported undefined behavior; see $log_file" >&2
    exit 1
  fi
}

run_logged() {
  log_file="$1"
  shift
  status_file="$log_file.status"
  rm -f "$status_file"
  set +e
  (
    set +e
    "$@" 2>&1
    printf '%s\n' "$?" > "$status_file"
  ) | tee "$log_file"
  tee_status="$?"
  set -e
  if [ "$tee_status" -ne 0 ]; then
    echo "error: tee failed while writing $log_file" >&2
    exit "$tee_status"
  fi
  if [ ! -f "$status_file" ]; then
    echo "error: command status was not captured for $log_file" >&2
    exit 1
  fi
  command_status=$(cat "$status_file")
  rm -f "$status_file"
  return "$command_status"
}

needs_sysinit_units() {
  # FPC 3.2+ i386-linux moved startup code from external loader files into
  # sysinit units (si_prc.pp etc.), which the bootstrap has to prebuild so
  # the later stages can use them.  x86_64-linux still ships rtl/linux/x86_64/
  # prt0.as, so use-fpc.sh can assemble the external startup object directly.
  [ "$TARGET_CPU" = "i386" ]
}

build_sysinit_units() {
  if ! needs_sysinit_units; then
    return 0
  fi
  pp_bin="$1"
  cfg_dir="$2"
  sysinit_dir="$3"
  echo "== build sysinit units for $pp_bin =="
  rm -rf "$sysinit_dir"
  mkdir -p "$sysinit_dir"
  mkdir -p "$SAN_LOGDIR"
  log_name=$(printf '%s' "sysinit-$sysinit_dir" | tr -c 'A-Za-z0-9_' '_')
  log_file="$SAN_LOGDIR/$log_name.log"
  echo "logging sysinit compiler output to $log_file"
  if ! run_logged "$log_file" \
      env PPC_CONFIG_PATH="$cfg_dir" \
        FPCDIR="$CLEAN_SRC" \
        "$pp_bin" -B -a -s \
          "-FE$sysinit_dir" \
          "-FU$sysinit_dir" \
          "$CLEAN_SRC/rtl/linux/si_prc.pp"
  then
    exit 1
  fi
  fail_on_ubsan_reports "$log_file"
  (
    cd "$sysinit_dir"
    sh ./ppas.sh
  )
}

stage_extra_unit_flags() {
  sysinit_dir="$1"
  if needs_sysinit_units && [ -d "$sysinit_dir" ]; then
    printf '%s ' "-Fu$sysinit_dir"
  fi
}

stage_force_build() {
  # FPC 3 i386 sysinit is prebuilt above so late unit injection loads
  # si_prc.ppu. Passing -B here would force source compilation again and
  # change the stage module list.
  if needs_sysinit_units; then
    printf '0'
  else
    printf '1'
  fi
}

build_stage1() {
  echo "== [3/7] translate FPC 3 compiler to C++ (TARGET_CPU=$TARGET_CPU) =="
  rm -rf "$STAGE1_DIR" "$STAGE1_LOGDIR"
  mkdir -p "$STAGE1_DIR"

  # Provide the conditional-compile macros that the Pascal compiler defines
  # for the selected target. tp2cc evaluates conditional compilation while
  # translating the sources, before the translated compiler's option parser
  # can call `def_system_macro' / `set_system_macro' at runtime, so these
  # definitions must be present here to select the same source branches.
  #
  # The shared + per-CPU lists model the source branches selected by FPC's
  # runtime option parser for the chosen stage1 target.  Keep them explicit:
  # emitting a translated compiler runs before options.pas can define these
  # symbols itself.

  # tp2cc-specific knobs: prune target backends we don't translate, and
  # force the dwarf debug-info backend off (it pulls array-of-const
  # parsing tp2cc doesn't yet handle).  The NASM/OMF gate is per-CPU:
  # NOAG386NSM on i386, NOAGX86_64NSM on x86_64; fpc_stage1_cpu_defines
  # already emits the right one, so this list does not.
  tp2cc_target_prune="\
-dNoDbgDwarf \
-dNOTARGETAIX -dNOTARGETAMIGA -dNOTARGETAROS -dNOTARGETATARI \
-dNOTARGETBEOS -dNOTARGETBSD -dNOTARGETDARWIN -dNOTARGETEMX \
-dNOTARGETFREEBSD -dNOTARGETGO32V1 -dNOTARGETGO32V2 \
-dNOTARGETHAIKU -dNOTARGETMACOS -dNOTARGETMORPHOS \
-dNOTARGETMSDOS -dNOTARGETNATIVENT -dNOTARGETNDS \
-dNOTARGETNETBSD -dNOTARGETNETWARE -dNOTARGETNETWLIBC \
-dNOTARGETOPENBSD -dNOTARGETOS2 -dNOTARGETPALMOS \
-dNOTARGETQNX -dNOTARGETSUNOS -dNOTARGETSYMBIAN \
-dNOTARGETWATCOM -dNOTARGETWDOSX -dNOTARGETWII \
$(fpc_target_prune_win_defines) \
-dNOTARGETANDROID -dNOTARGETGBA -dNOTARGETEMBEDDED"

  "$HOST_BUILD/bin/tp2cc" emit-all \
    $(fpc_stage1_shared_defines) \
    $(fpc_stage1_source_version_defines) \
    $(fpc_stage1_cpu_defines) \
    $tp2cc_target_prune \
    $(compiler_dir_flags) \
    -Fi"$CLEAN_SRC/rtl/inc" \
    -Fi"$CLEAN_SRC/rtl/$TARGET_CPU" \
    -Fi"$CLEAN_SRC/rtl/linux" \
    "$CLEAN_SRC/compiler/pp.pas" "$STAGE1_DIR"
  write_bootstrap_cfg
  install_support_files "$STAGE1_DIR"

  echo "== [4/7] compile and link translated stage1 compiler =="
  JOBS="$JOBS" CXX="$CXX" LOGDIR="$STAGE1_LOGDIR" \
    tools/compile_emitted.sh "$STAGE1_DIR"
  if grep -q '^FAIL ' "$STAGE1_LOGDIR/build.txt"; then
    cat "$STAGE1_LOGDIR/summary.txt" >&2
    exit 1
  fi
  (
    cd "$STAGE1_DIR"
    "$CC" $CFLAGS -c "$RUNTIME_SHIM" -o tp2cc_fenv_shim.o
    "$CXX" -O3 -g3 $SAN p_*.o tp2cc_fenv_shim.o -lm -o pp
  )
}

compile_pp_stage() {
  stage_name="$1"
  pp_bin="$2"
  cfg_dir="$3"
  out_dir="$4"

  echo "== [$stage_name] compile compiler/pp.pas =="
  rm -rf "$out_dir"
  mkdir -p "$out_dir"
  sysinit_dir="$out_dir-sysinit-units"
  mkdir -p "$SAN_LOGDIR"
  build_sysinit_units "$pp_bin" "$cfg_dir" "$sysinit_dir"
  log_name=$(printf '%s' "$stage_name" | tr -c 'A-Za-z0-9_' '_')
  log_file="$SAN_LOGDIR/$log_name.log"
  echo "logging $stage_name compiler output to $log_file"
  if ! run_logged "$log_file" \
      env PP="$pp_bin" \
        PPC_CONFIG_PATH="$cfg_dir" \
        FPCDIR="$CLEAN_SRC" \
        STARTUP_AS="$STARTUP_AS" \
        STARTUP_ASFLAGS="$STARTUP_ASFLAGS" \
        USE_FPC_FORCE_BUILD="$(stage_force_build)" \
      "$ROOT/use-fpc.sh" \
        $(compiler_dir_flags) \
        $(stage_extra_unit_flags "$sysinit_dir") \
        "-o$out_dir/pp" \
        "$CLEAN_SRC/compiler/pp.pas"
  then
    exit 1
  fi
  fail_on_ubsan_reports "$log_file"
  install_support_files "$out_dir"
}

verify_compiler() {
  pp_bin="$1"
  cfg_dir="$2"
  help_out=$(mktemp "${TMPDIR:-/tmp}/tp2cc-bootstrap-help.XXXXXX")
  if PPC_CONFIG_PATH="$cfg_dir" FPCDIR="$CLEAN_SRC" "$pp_bin" -h >"$help_out" 2>&1; then
    :
  fi
  if grep -q ': runtime error:' "$help_out"; then
    cat "$help_out" >&2
    rm -f "$help_out"
    exit 1
  fi
  if ! grep -q "Free Pascal Compiler version" "$help_out"; then
    cat "$help_out" >&2
    rm -f "$help_out"
    exit 1
  fi
  rm -f "$help_out"
}

if [ ! -d "$SOURCE_DIR" ]; then
  echo "error: FPC 3 source tree not found: $SOURCE_DIR" >&2
  exit 1
fi

if [ ! -f "$ENTRY_FILE" ]; then
  echo "error: compiler entry file not found: $ENTRY_FILE" >&2
  exit 1
fi

if [ ! -f "$MSG_FILE" ]; then
  echo "error: message file not found: $MSG_FILE" >&2
  exit 1
fi

if [ ! -f "$RUNTIME_SHIM" ]; then
  echo "error: runtime shim not found: $RUNTIME_SHIM" >&2
  exit 1
fi

build_translator
copy_clean_source
build_stage1

compile_pp_stage "5/7" "$STAGE1_DIR/pp" "$STAGE1_DIR" "$STAGE2_DIR"
verify_compiler "$STAGE2_DIR/pp" "$STAGE2_DIR"

compile_pp_stage "6/7" "$STAGE2_DIR/pp" "$STAGE2_DIR" "$STAGE3_DIR"
verify_compiler "$STAGE3_DIR/pp" "$STAGE3_DIR"

echo "== [7/7] bootstrap summary =="
echo "stage1: $STAGE1_DIR/pp"
echo "stage2: $STAGE2_DIR/pp"
echo "stage3: $STAGE3_DIR/pp"
