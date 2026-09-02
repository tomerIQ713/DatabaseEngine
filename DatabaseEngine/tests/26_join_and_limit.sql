-- The paths the executor gained when it stopped considering every pair and
-- stopped ordering rows it was about to discard.
create table emp (id int, name text, dept int);
create table dept (id int, label text);
insert into emp values (1, 'ann', 10);
insert into emp values (2, 'bob', 20);
insert into emp values (3, 'cid', 10);
insert into emp values (4, 'dee', null);
insert into emp values (5, 'eve', 99);
insert into dept values (10, 'eng');
insert into dept values (20, 'ops');
insert into dept values (10, 'eng-two');
insert into dept values (null, 'nowhere');

-- an equality across two tables is a hash join
.explain on
select emp.name, dept.label from emp, dept where emp.dept = dept.id;
-- written the other way round, and as a JOIN ... ON
select emp.name, dept.label from emp, dept where dept.id = emp.dept;
select emp.name, dept.label from emp join dept on emp.dept = dept.id;
-- a NULL key joins nothing, on either side
select count(*) from emp, dept where emp.dept = dept.id;
-- an unmatched key drops the row: eve's 99 and dee's NULL are not in dept
select emp.name from emp, dept where emp.dept = dept.id;
-- a second predicate still applies
select emp.name, dept.label from emp, dept where emp.dept = dept.id and emp.id > 1;
-- under OR there is nothing to key on, so the nested loop stays
select count(*) from emp, dept where emp.dept = dept.id or emp.id = 5;
-- an inequality is not a hash key either
select count(*) from emp, dept where emp.dept > dept.id;
-- text keys
select e.name, d.label from emp e, dept d where e.name = d.label;
-- a table joined to itself
select x.name, y.name from emp x, emp y where x.dept = y.dept;
.explain off

-- LIMIT with no ORDER BY stops the scan; the rows are the first ones stored
select id, name from emp limit 2;
select id, name from emp where dept = 10 limit 1;
select id from emp limit 0;
select id from emp limit 99;

-- LIMIT with an ORDER BY keeps only the best few, and must agree with the
-- same query without the limit
select id, name from emp order by name desc;
select id, name from emp order by name desc limit 3;
select dept, count(*) from emp group by dept order by dept limit 2;

-- ties keep the order the rows arrived in, so this is the same every run
select dept, name from emp order by dept;
select distinct dept from emp order by dept;
