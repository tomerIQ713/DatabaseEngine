-- an index survives save/load as pages, instead of being rebuilt from the rows
create table t (id int, name text, grp int);
insert into t values (5, 'eee', 1);
insert into t values (1, 'aaa', 1);
insert into t values (9, 'iii', 2);
insert into t values (-3, 'ccc', 2);
insert into t values (7, 'ggg', 1);
create index t_id on t (id);
create index t_name on t (name);
.indexes
select id from t where id = 9;
select id from t where id < 0;
select id from t where id >= 5 order by id;
select name from t where name = 'ccc';
select name from t where name like 'g%';
.save tmp_test.db
.load tmp_test.db
.indexes
select id from t where id = 9;
select id from t where id < 0;
select id from t where id >= 5 order by id;
select name from t where name = 'ccc';
select name from t where name like 'g%';
insert into t values (2, 'bbb', 3);
select id from t where id = 2;
delete from t where id = 5;
select id from t where id >= 5 order by id;
.indexes
vacuum t;
.indexes
select id from t where id >= 5 order by id;
select id from t where id = 2;
.save tmp_test.db
.load tmp_test.db
select id from t where id = 2;
select count(*) from t;
drop index t_name;
.indexes
drop table t;
.indexes
