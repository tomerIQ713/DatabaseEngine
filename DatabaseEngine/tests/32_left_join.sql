-- LEFT JOIN: a row of the left table survives whether or not the ON matched.

create table users (id int, name text);
insert into users values (1, 'ada');
insert into users values (2, 'bo');
insert into users values (3, 'cy');

create table orders (uid int, item text, total int);
insert into orders values (1, 'book', 250);
insert into orders values (1, 'pen', 90);
insert into orders values (3, 'lamp', 120);

-- bo ordered nothing and is kept, padded with NULL
select users.name, orders.item from users left join orders on users.id = orders.uid;
select users.name, orders.item from users left outer join orders on users.id = orders.uid;

-- the inner join drops bo, which is the whole difference
select users.name, orders.item from users join orders on users.id = orders.uid;

-- count skips NULL, so bo counts zero rather than one
select users.name, count(orders.item) from users left join orders on users.id = orders.uid group by users.name;

-- ON decides what pairs; WHERE filters what came out. bo survives the first
-- and not the second, which is the distinction the two clauses exist for.
select users.name, orders.item from users left join orders on users.id = orders.uid and orders.total > 100;
select users.name, orders.item from users left join orders on users.id = orders.uid where orders.total > 100;

-- the anti-join: rows of the left table with no partner at all
select users.name from users left join orders on users.id = orders.uid where orders.item is null;

-- chained, so the NULLs of one level reach the next
create table a (id int, x text);
insert into a values (1, 'a1');
insert into a values (2, 'a2');
create table b (id int, aid int, y text);
insert into b values (10, 1, 'b1');
create table c (id int, bid int, z text);
insert into c values (100, 10, 'c1');
select a.x, b.y, c.z from a left join b on a.id = b.aid left join c on b.id = c.bid;

-- and it still orders, groups and limits like any other result
select a.x, b.y from a left join b on a.id = b.aid order by a.x desc;
select count(*) from a left join b on a.id = b.aid;

-- an outer join still needs its ON
select a.x from a left join b;
