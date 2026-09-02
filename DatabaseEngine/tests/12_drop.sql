create table t (id int, name text)
insert into t values (1, 'a')
insert into t values (2, 'b')
create index idx on t (id)
create index idx2 on t (name)
.tables
.indexes
drop index idx
.indexes
select * from t where id = 1
drop index idx
drop index nope
select count(*) from t
drop table t
.tables
.indexes
select * from t
drop table nope
create table t (id int)
.tables
select count(*) from t
insert into t values (7)
select * from t
drop table
drop t
drop index
.exit
