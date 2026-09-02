create table sales (dept text, region text, amount int)
insert into sales values ('eng', 'north', 100)
insert into sales values ('eng', 'south', 250)
insert into sales values ('ops', 'north', 75)
insert into sales values ('ops', 'north', 25)
insert into sales values ('hr', 'south', 400)
select dept, count(*) from sales group by dept
select dept, count(*), sum(amount), min(amount), max(amount) from sales group by dept
select dept, region, count(*) from sales group by dept, region
select count(*) from sales
select dept, amount from sales group by dept
select dept, count(*) from sales
select * from sales group by dept
select sum(dept) from sales
select avg(amount) from sales
select sum(*) from sales
select count(*) from sales group dept
.exit
