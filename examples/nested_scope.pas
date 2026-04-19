program nesteddemo;

function MakeAdder(base: integer): integer;
  function AddInner(v: integer): integer;
  begin
    AddInner := base + v;
  end;
begin
  MakeAdder := AddInner(7);
end;

procedure CountDown(n: integer);
  procedure Step(k: integer);
  begin
    if k > 0 then
      begin
        write(k, ' ');
        Step(k - 1);
      end;
  end;
begin
  Step(n);
  writeln('go');
end;

begin
  writeln('sum=', MakeAdder(5));
  CountDown(3);
end.
