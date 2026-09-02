create table t (name text, amount int)
insert into t values ('a', 10)
insert into t values ('b', null)
insert into t values (null, 30)
insert into t values (null, null)
insert into t values ('a', 5)
select * from t
select * from t where amount is null
select * from t where amount is not null
select * from t where amount > 5
select * from t where amount = null
select count(*), count(amount), sum(amount), min(amount), max(amount) from t
select name, count(*), count(amount), sum(amount) from t group by name
.exit
