-- Arithmetic wherever a value can be read.
create table line (id int, qty int, price float, note text);
insert into line values (1, 4, 2.5, 'a');
insert into line values (2, 3, 10.0, 'b');
insert into line values (3, 10, 1.25, 'c');
insert into line values (4, 7, NULL, NULL);

-- an item is named after what it says, unless AS says otherwise
select id, qty * 2, price + 1 from line;
select id, qty * price as total from line;

-- precedence, and brackets that survive into the header
select id, qty + 2 * 3 from line where id = 1;
select id, (qty + 2) * 3 from line where id = 1;
select id, -qty, - -qty from line where id = 1;

-- int stays int, and anything touching a float is a float
select id, qty / 3, qty % 3, qty / 3.0 from line;

-- both sides of a comparison are expressions
select id from line where qty * price > 10;
select id from line where qty + 1 > qty;
select id from line where 20 = qty * 2;
select id from line where qty * 2 = qty + qty;

-- NULL spreads through arithmetic, and an unknown comparison keeps its row out
select id, qty * price from line where id = 4;
select id from line where qty * price > 0;

-- aggregates take expressions
select sum(qty * price), avg(qty * price), min(qty * 2), max(qty + 1) from line;
select count(*) from line where qty % 2 = 0;

-- and so does GROUP BY's output, through the label
select note, sum(qty * price) from line where note is not null group by note order by sum(qty * price) desc;

-- ordering by an expression that is in the result
select id, qty * price from line order by qty * price desc;
-- but not by one that is not: ORDER BY names output columns
select id from line order by qty * price;

-- what arithmetic refuses
select note * 2 from line;
select id / 0 from line;
select id % 0 from line;
select 2147483647 + 1 from line;

-- UPDATE computes from the row it is updating
update line set qty = qty * 10 where id = 1;
select id, qty from line where id = 1;
-- every assignment sees the row as it was, so this swaps rather than copies
create table pair (a int, b int);
insert into pair values (1, 2);
update pair set a = b, b = a;
select * from pair;

-- an index cannot seek to an expression, so the plan falls back to a scan
create index line_qty on line (qty);
.explain on
select id from line where qty = 3;
select id from line where qty * 1 = 3;
.explain off

-- a CHECK may hold one too
create table inv (id int, qty int, price float, check (qty * price <= 100.0));
insert into inv values (1, 2, 3.0);
insert into inv values (2, 50, 10.0);
select * from inv;
