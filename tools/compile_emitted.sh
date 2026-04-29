#!/bin/sh
# Compile every emitted *.cc file in parallel. Writes per-file logs,
# a status summary, and cleans up its own process group on signal/exit.
#
# Usage:  tools/compile_emitted.sh [emitted-dir]
# Env:    JOBS (default 5), CXX (default g++), CXXFLAGS, LOGDIR
#
# Job control:
#   On first run we re-exec ourselves via `setsid` so that $$ is also our
#   process-group id. Traps on INT/TERM/HUP then broadcast the signal to
#   the whole group with `kill -- -$$`, which cleanly reaps xargs plus
#   every g++ child. The EXIT trap does NOT kill the group -- otherwise we
#   would kill ourselves on normal completion.

set -u

# Re-exec in a new session/process group on first entry.
if [ -z "${TP2CC_SESSION-}" ]; then
  TP2CC_SESSION=1 exec setsid --wait "$0" "$@"
fi

DIR="${1:-build/emitted}"
JOBS="${JOBS:-5}"
DIR="$(cd "$DIR" && pwd)"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOGDIR="${LOGDIR:-$(dirname "$DIR")/compile-logs}"
TMPDIR="${TMPDIR:-$(dirname "$DIR")/tmp}"
mkdir -p "$LOGDIR"
mkdir -p "$TMPDIR"

CXX="${CXX:-g++}"
# -Wno-narrowing: Pascal freely uses `$80000000`-style hex constants as
# bitmasks that technically narrow to int32; emission keeps them as plain
# decimal and we suppress the warning rather than wrap every literal.
CXXFLAGS="${CXXFLAGS:--std=gnu++20 -I. -O0 -pipe -Wno-narrowing}"

STATUS="$LOGDIR/build.txt"
: > "$STATUS"

export CXX CXXFLAGS LOGDIR STATUS DIR ROOT TMPDIR

on_signal() {
  sig="$1"
  trap - INT TERM HUP
  # Negative PID -> signal the whole process group. We're the group
  # leader because we setsid'd.
  kill -TERM -- -$$ 2>/dev/null || true
  # Give children a moment to flush, then force-kill any stragglers.
  sleep 1
  kill -KILL -- -$$ 2>/dev/null || true
  exit $((128 + sig))
}
trap 'on_signal 2'  INT
trap 'on_signal 15' TERM
trap 'on_signal 1'  HUP

# Fan out: one `$CXX -c` per emitted .cc. xargs keeps at most $JOBS
# running at a time. Each inner shell cd's into DIR so `-I.` resolves.
( cd "$DIR" && ls p_*.cc ) \
  | xargs -n1 -P"$JOBS" sh -c '
      f="$1"
      base="${f%.cc}"
      cd "$DIR"
      if $CXX -I. -I"$ROOT" $CXXFLAGS -c "$f" -o "$base.o" 2>"$LOGDIR/$base.log"; then
        rm -f "$LOGDIR/$base.log"
        echo "OK   $base" >> "$STATUS"
      else
        echo "FAIL $base" >> "$STATUS"
      fi
    ' sh

ok=$(grep -c "^OK"   "$STATUS" 2>/dev/null || echo 0)
fail=$(grep -c "^FAIL" "$STATUS" 2>/dev/null || echo 0)

{
  echo "=== compile summary ==="
  echo "ok:     $ok"
  echo "failed: $fail"
  echo
  echo "=== first error per failing unit ==="
  # Iterate only units that failed in this run (build.txt FAIL lines)
  # so stale logs from previously-emitted units that are no longer
  # part of the build (e.g. a skipped pascal source) don't appear.
  grep "^FAIL" "$STATUS" | awk '{print $2}' | while read u; do
    lg="$LOGDIR/$u.log"
    [ -f "$lg" ] || continue
    first=$(grep -m1 -E "error:" "$lg" | sed 's|^[^:]*:[0-9]*:[0-9]*: ||')
    echo "  $u: $first"
  done | sort
} > "$LOGDIR/summary.txt"

echo "wrote $LOGDIR/summary.txt  ($ok ok, $fail failed)"
