create table t (id int, name text)
insert into t values (3, 'banana')
insert into t values (1, 'apple')
insert into t values (4, 'cherry')
insert into t values (2, 'avocado')
insert into t values (5, null)
select * from t order by id
select * from t order by id desc
select * from t order by name
select * from t order by name desc
select * from t order by name asc
select id, name from t order by name, id
select * from t order by bogus
select * from t order id
.exit
