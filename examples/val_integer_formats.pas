program testval;

{
  This example is intentionally version-sensitive.

  Observed with FPC 3.2.2:
    '&77' gives code=0, longint=63.
    '2147483648' gives code=0, longint=-2147483648.

  In the 0.99.14 source tree bundled here, rtl/inc/sstrings.inc says
  otherwise:
    - InitVal only recognizes '$' and '%' prefixes.
    - ValSignedInt treats positive base-10 overflow as an error.
    - The only decimal overflow special case is '-2147483648'.

  So for this tree the expected results are:
    '&77' gives code=1, longint=0.
    '2147483648' gives code=10, longint=0.
}

procedure TryVal(const s: string);
var
  l: longint;
  c: integer;
begin
  val(s, l, c);
  writeln('s="', s, '"  code=', c, '  longint=', l);
end;

begin
  TryVal('$7fffffff');
  TryVal('$80000000');
  TryVal('$D7B0');
  TryVal('%1010');
  TryVal('&77');
  TryVal('2147483647');
  TryVal('2147483648');
end.
