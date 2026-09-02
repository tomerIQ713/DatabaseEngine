create table t (id int, name text)
insert into t values (10, 'a')
insert into t values (20, 'b')
insert into t values (30, 'c')
insert into t values (40, 'd')
create index idx on t (id)
delete from t where id = 20
select * from t order by id
select * from t where id = 20
update t set id = 99 where id = 30
select * from t order by id
select * from t where id = 30
select * from t where id = 99
update t set name = 'Z', id = 1 where name = 'a'
select * from t order by id
update t set id = 1, id = 2
update t set bogus = 1
delete from nope where id = 1
.indexes
select count(*) from t
vacuum t
.indexes
select count(*) from t
select * from t order by id
select * from t where id = 99
vacuum nope
.exit
