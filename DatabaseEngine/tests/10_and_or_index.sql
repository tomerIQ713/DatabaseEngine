create table t (id int, name text, tag text)
insert into t values (1, 'apple', 'red')
insert into t values (2, 'banana', 'yellow')
insert into t values (3, 'cherry', 'red')
insert into t values (4, 'avocado', 'green')
create index idx on t (id)
create index idx_name on t (name)
.explain on
select * from t where id = 3
select * from t where id = 3 and tag = 'red'
select * from t where tag = 'red' and id = 3
select * from t where id >= 2 and tag = 'red'
select * from t where id = 3 or tag = 'red'
select * from t where not id = 3
select * from t where tag = 'red' and (id = 1 or id = 3)
select * from t where id = 1 and id = 2
select * from t where name like 'a%' and tag = 'red'
select * from t where tag = 'red' and name like 'a%'
select * from t where name like '%a' and id > 0
.exit
