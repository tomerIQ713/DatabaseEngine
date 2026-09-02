-- Subqueries: IN, NOT IN, EXISTS, and a scalar on the right of an operator.
-- All uncorrelated, so each runs once before the outer query starts.

create table users (id int, name text, city text);
insert into users values (1, 'ada', 'haifa');
insert into users values (2, 'bo', 'eilat');
insert into users values (3, 'cy', 'haifa');

create table orders (uid int, total int);
insert into orders values (1, 250);
insert into orders values (1, 90);
insert into orders values (3, 120);

create table big (n int);
insert into big values (100);

-- IN against a subquery, and its negation
select name from users where id in (select uid from orders);
select name from users where id not in (select uid from orders);

-- IN against a value list, which is rewritten into an OR of equalities
select name from users where id in (1, 3);
select name from users where id not in (1, 3);
select name from users where city in ('haifa');

-- a scalar subquery on the right of an operator
select name from users where id = (select max(uid) from orders);
select name, city from users where id < (select count(*) from orders);

-- EXISTS asks only whether anything came back
select name from users where exists (select uid from orders);
select name from users where exists (select uid from orders where total > 9999);
select name from users where not exists (select uid from orders where total > 9999);

-- nested: the inner one runs first
select name from users where id in (select uid from orders where total > (select n from big));

-- with a join above it
select users.name, orders.total from users join orders on users.id = orders.uid where orders.total > (select n from big);

-- in a delete and an update
update users set city = 'moved' where id in (select uid from orders where total > 200);
select * from users;
delete from users where id not in (select uid from orders);
select * from users;

-- NULL is not a value that can be matched, so a set holding one answers
-- "unknown" rather than "no" - and NOT of unknown is still unknown.
create table t (id int, v int);
insert into t values (1, 10);
insert into t values (2, null);
create table s (k int);
insert into s values (10);
insert into s values (null);
select id from t where v in (select k from s);
select id from t where v not in (select k from s);

-- what a subquery may not do here
select name from users where id = (select uid from orders);
select name from users where id in (select uid, total from orders);
select name from users where id in (select uid from nosuch);
select name from users where exists (select uid from orders where orders.uid = users.id);
