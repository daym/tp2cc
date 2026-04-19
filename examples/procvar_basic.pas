program procvardemo;

type
  TFunc = function(x: integer): integer;

function ApplyTwice(f: TFunc; x: integer): integer;
begin
  ApplyTwice := f(f(x));
end;

function Inc1(x: integer): integer;
begin
  Inc1 := x + 1;
end;

begin
  writeln(ApplyTwice(@Inc1, 5));
end.
