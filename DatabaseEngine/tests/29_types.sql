-- float, date and varchar(n), and avg, which is the reason float exists.
create table readings (id int, taken date, celsius float, note varchar(10));

insert into readings values (1, '2024-01-31', 3.5, 'cold');
insert into readings values (2, '2024-02-29', -1.25, 'freezing');
insert into readings values (3, '2023-12-25', 20, 'warm');
insert into readings values (4, '2024-06-01', NULL, NULL);
select * from readings;

-- an int literal stands in for a float, and a date is written as text
select id, celsius from readings where celsius > 3;
select id, taken from readings where taken >= '2024-01-01';
select id, taken from readings order by taken;
select id, taken from readings order by taken desc;

-- dates are validated, not just stored
insert into readings values (5, '2023-02-29', 1.0, 'no');
insert into readings values (5, '2024-13-01', 1.0, 'no');
insert into readings values (5, '2024-1-1', 1.0, 'no');
insert into readings values (5, 'yesterday', 1.0, 'no');

-- varchar(10) is text with a ceiling
insert into readings values (5, '2024-06-02', 1.0, 'a note that will not fit');
insert into readings values (5, '2024-06-02', 1.0, 'exactly 10');

-- avg is a real number even over ints, and skips NULLs
select avg(celsius), sum(celsius), min(celsius), max(celsius), count(celsius) from readings;
select avg(id), sum(id) from readings;
select avg(celsius) from readings where id > 100;

-- grouping and ordering by a float
create table sales (region text, amount float);
insert into sales values ('north', 1.5);
insert into sales values ('north', 2.25);
insert into sales values ('south', 10.0);
select region, avg(amount), sum(amount) from sales group by region order by avg(amount) desc;

-- a float column cannot be summed as text, and text cannot be averaged
select avg(region) from sales;

-- an index on a float, and one on a date: both are seeks, not scans
create index readings_c on readings (celsius);
create index readings_t on readings (taken);
.indexes
.explain on
select id from readings where celsius = 20;
select id from readings where celsius > 0;
select id from readings where taken = '2024-01-31';
select id from readings where taken < '2024-01-01';
.explain off

-- and the index answers what the scan answers: same rows, ordered the same way
-- on purpose, because the access path decides what order they arrive in
select id from readings where celsius > 0 order by id;
drop index readings_c;
select id from readings where celsius > 0 order by id;
