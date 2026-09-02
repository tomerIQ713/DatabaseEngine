create table t (id int, name text, tag text)
insert into t values (1, 'apple', 'red')
insert into t values (2, 'banana', 'yellow')
insert into t values (3, 'cherry', 'red')
insert into t values (4, 'avocado', 'green')
insert into t values (5, null, 'red')
select * from t where id > 1 and tag = 'red'
select * from t where id = 1 or id = 4
select * from t where id = 1 or id = 4 or id = 2
select * from t where tag = 'red' and id < 3 or tag = 'green'
select * from t where tag = 'red' and (id < 3 or tag = 'green')
select * from t where (tag = 'red' or tag = 'green') and id > 2
select * from t where not tag = 'red'
select * from t where not (id = 1 or id = 2)
select * from t where not not id = 1
select * from t where id > 1 and id < 4 and tag = 'red'
select * from t where name is null or id = 1
select * from t where name is not null and name like 'a%'
select * from t where not name like 'a%'
select * from t where name like 'a%' and name is null
select id from t where id = 1 and tag = 'red' order by id
delete from t where id = 2 or id = 4
select * from t order by id
update t set tag = 'x' where id = 1 and name like 'app%'
select * from t order by id
select * from t where id = 1 and
select * from t where id = 1 or
select * from t where (id = 1
select * from t where id = 1 or bogus = 2
select * from t where and id = 1
.exit
