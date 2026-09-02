create table t (id int, name text)
insert into t values (1, 'apple')
insert into t values (2, 'apricot')
insert into t values (3, 'banana')
insert into t values (4, 'avocado')
insert into t values (5, 'application')
insert into t values (6, null)
select name from t where name like '%a%' order by name
select name from t where name like 'ap%' order by name
select name from t where name like '%a' order by name
select name from t where name like '_p%' order by name
select name from t where name like 'apple'
select name from t where name like '%' order by name
select name from t where name like 'A%'
select name from t where name like '%an%na%'
select name from t where name like 'c_erry'
select name from t where name not like 'ap%' order by name
select name from t where name not like '%a%' order by name
select * from t where id like '3'
select * from t where name like 5
.exit
