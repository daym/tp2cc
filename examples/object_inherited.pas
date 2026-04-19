program objectdemo;

type
  TBase = object
    x: integer;
    procedure Init(v: integer);
    function Twice: integer;
  end;

  TChild = object(TBase)
    y: integer;
    procedure Init2(a, b: integer);
    function Sum: integer;
  end;

procedure TBase.Init(v: integer);
begin
  x := v;
end;

function TBase.Twice: integer;
begin
  Twice := x * 2;
end;

procedure TChild.Init2(a, b: integer);
begin
  inherited Init(a);
  y := b;
end;

function TChild.Sum: integer;
begin
  Sum := x + y;
end;

var
  c: TChild;

begin
  c.Init2(3, 4);
  writeln('twice=', c.Twice, ' sum=', c.Sum);
end.
