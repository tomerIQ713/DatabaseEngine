create table t (id int, name text)
insert into t values (1, 'apple')
insert into t values (2, 'banana')
insert into t values (3, 'cherry')
select * from t where id = 2
select * from t where id <> 2
select * from t where id != 2
select * from t where id < 2
select * from t where id <= 2
select * from t where id > 2
select * from t where id >= 2
select * from t where name = 'banana'
select * from t where name = 'BANANA'
select * from t where id = 'x'
select * from t where bogus = 1
select * from t where id
select * from t where id = 1 extra
.exit
