create table sales (dept text, rep text, amount int)
insert into sales values ('eng', 'Tomer', 100)
insert into sales values ('eng', 'Dana', 200)
insert into sales values ('ops', 'Noa', 300)
insert into sales values ('ops', 'Gil', 50)
insert into sales values ('hr', 'Maya', 400)
insert into sales values ('hr', 'Ron', null)
insert into sales values ('hr', 'Ann', null)
select * from sales order by amount desc limit 3
select * from sales order by amount limit 2
select * from sales limit 0
select * from sales order by amount limit 99
select * from sales limit -1
select * from sales limit x
select distinct dept from sales order by dept
select distinct amount from sales order by amount
select distinct dept, amount from sales order by dept, amount
select distinct * from sales order by rep limit 3
select distinct bogus from sales
select dept, count(*) from sales group by dept having count(*) > 2
select dept, sum(amount) from sales group by dept having sum(amount) > 250
select dept, count(*) from sales group by dept having dept = 'eng'
select dept, count(*) from sales group by dept having count(*) > 1 order by dept
select dept, count(*) from sales group by dept having count(*) > 1 limit 1
select dept, count(*), sum(amount) from sales group by dept having count(*) > 1 and sum(amount) > 300
select dept, count(*) from sales group by dept having count(*) = 2 or dept = 'hr'
select count(*) from sales having count(*) > 1
select dept from sales having count(*) > 1
select * from sales having count(*) > 1
select dept, count(*) from sales group by dept having bogus > 1
select dept, count(*) from sales group by dept having count(*) > 'x'
.exit
