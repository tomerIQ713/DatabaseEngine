create table t (id int, name text)
insert into t values (30, 'c')
insert into t values (10, 'a')
insert into t values (20, 'b')
insert into t values (20, 'd')
create index idx on t (id)
create index idx on t (id)
create index i2 on t (bogus)
create index i3 on nope (id)
.indexes
.explain on
select * from t where id = 20
select * from t where id >= 20
select * from t where id < 20
select * from t where name = 'a'
.explain off
create index idx_name on t (name)
select name from t where name like 'a%'
.indexes
.exit
