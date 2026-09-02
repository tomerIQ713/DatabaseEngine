create table users (id int, name text, note text)
insert into users values (1, 'Tomer', 'hello')
insert into users values (2, 'Dana', null)
insert into users values (-42, null, 'it''s quoted')
insert into users values (7, 'Gone', 'deleted soon')
create table empty (k int)
create index idx on users (id)
create index idx_name on users (name)
delete from users where id = 7
update users set note = 'updated' where id = 1
.tables
.indexes
select * from users order by id
.save tmp_test.db
drop table users
drop table empty
.tables
.indexes
.load tmp_test.db
.tables
.indexes
select * from users order by id
select count(*) from empty
select * from users where id = -42
select * from users where name = 'Dana'
select * from users where name is null
select * from users where note like 'it%'
insert into users values (5, 'New', 'after reload')
select count(*) from users
.load no_such_file.db
.tables
select count(*) from users
.exit
