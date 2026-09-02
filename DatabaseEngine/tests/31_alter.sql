-- ALTER TABLE: add, drop, rename column, rename table.

create table t (id int, name text);
insert into t values (1, 'ada');
insert into t values (2, 'bo');

-- a new column reaches rows that were written before it existed
alter table t add column score float default 1.5;
select * from t;

-- with no default those rows get NULL, which is what "unknown" means here
alter table t add column note text;
select * from t;
insert into t values (3, 'cy', 9.5, 'hi');
select * from t;

-- dropping shifts every later column down, and the index on one must follow
create index ins on t (score);
create index inn on t (note);
alter table t drop column name;
.indexes
.explain on
select * from t where note = 'hi';
select * from t where score = 1.5;
.explain off

-- names only: no row is touched, because a record stores slots not names
alter table t rename column note to memo;
select id, memo from t;

alter table t rename to people;
select * from people;
.tables
.indexes

-- what alter refuses, and why
alter table people add column id int;
alter table people add column u int unique;
alter table people add column k int primary key;
alter table people add column n int not null;
alter table people drop column nope;
alter table people rename column nope to other;
alter table people rename to people;
alter table nosuch add column x int;

create table only (x int);
alter table only drop column x;

create table guarded (a int, b int check (b > 0));
alter table guarded drop column b;
alter table guarded rename column b to c;

-- not null is fine when every existing row can be given a value
alter table people add column flag int not null default 0;
select id, flag from people;
