#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
WORK_DIR="${WORK_DIR:-$ROOT/build/rpm-smoke}"
STATUS=0

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

run_fixnasm() {
  dir="$WORK_DIR/fixnasm"
  mkdir -p "$dir"
  cp "$ROOT/../rpm/compiler/utils/fixnasm.pp" "$dir/fixnasm.pp"
  cat > "$dir/insns.dat" <<'EOF'
mov reg,reg 89 /r
add reg,imm 83 /0 ib
; comment
EOF
  cat > "$dir/expected.txt" <<'EOF'

[mov]
(Ch_All, Ch_None, Ch_None)
reg,reg               89                              /r

[add]
(Ch_All, Ch_None, Ch_None)
reg,imm               83                              /0
; comment
EOF

  echo "==> fixnasm"
  (
    cd "$dir"
    "$ROOT/use-fpc.sh" fixnasm.pp
    ./fixnasm > stdout.txt
  )
  if ! cmp -s "$dir/expected.txt" "$dir/insns.new"; then
    echo "mismatch: fixnasm output" >&2
    diff -u "$dir/expected.txt" "$dir/insns.new" >&2 || true
    STATUS=1
    return
  fi
  echo "ok: fixnasm"
}

run_fixlog() {
  dir="$WORK_DIR/fixlog"
  mkdir -p "$dir"
  cp "$ROOT/../rpm/compiler/utils/fixlog.pp" "$dir/fixlog.pp"
  cat > "$dir/sample.pas" <<'EOF'
{
  $Log: sample.pas,v $
  Revision 1.3  2000/01/08 13:52:02  peter
    newest

  Revision 1.2  2000/01/07 01:15:00  peter
    older

  Revision 1.1  1999/10/06 06:29:03  peter
    oldest
}
program sample;
begin
end.
EOF
  cat > "$dir/expected.pas" <<'EOF'
{
  $Log: sample.pas,v $
  Revision 1.3  2000/01/08 13:52:02  peter
    newest

  Revision 1.2  2000/01/07 01:15:00  peter
    older

}
program sample;
begin
end.
EOF

  echo "==> fixlog"
  (
    cd "$dir"
    "$ROOT/use-fpc.sh" fixlog.pp
    ./fixlog 2 2000/01/07 sample.pas > stdout.txt
  )
  if ! cmp -s "$dir/expected.pas" "$dir/sample.pas"; then
    echo "mismatch: fixlog output" >&2
    diff -u "$dir/expected.pas" "$dir/sample.pas" >&2 || true
    STATUS=1
    return
  fi
  echo "ok: fixlog"
}

run_fixnasm
run_fixlog

exit "$STATUS"
