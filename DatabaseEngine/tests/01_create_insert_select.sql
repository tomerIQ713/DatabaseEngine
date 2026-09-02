create table users (id int, name text)
create table users (id int)
create table t ()
create table t (id)
create table t (id bogus)
.tables
insert into users values (1, 'Tomer')
insert into users values (2, 'Dana')
insert into users values (3)
insert into users values ('x', 'y')
insert into users values (1, 'a', 'b')
insert into nope values (1, 'a')
select * from users
select name from users
select name, id from users
select bogus from users
select * from nope
select * from users extra junk
.exit
