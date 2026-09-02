-- PRIMARY KEY, UNIQUE, NOT NULL, DEFAULT and CHECK, and the column list that
-- makes a DEFAULT reachable.
create table users (id int primary key, email text unique, name varchar(6) not null, role text default 'user', age int default 0 check (age >= 0), check (age < 150));
.tables
.indexes

insert into users values (1, 'a@x', 'ada', 'admin', 36);
insert into users (id, email, name) values (2, 'b@x', 'bo');
insert into users (name, id) values ('cy', 3);
select * from users;

-- a primary key is unique
insert into users values (1, 'z@x', 'zed', 'user', 1);
-- and not null
insert into users (id, name) values (NULL, 'nil');
-- so is a plain unique column
insert into users (id, email, name) values (4, 'a@x', 'dup');
-- but NULLs never collide with each other
insert into users (id, name) values (5, 'eve');
insert into users (id, name) values (6, 'fay');
select count(*) from users where email is null;

-- not null
insert into users (id, name) values (7, NULL);
-- varchar(6)
insert into users (id, name) values (7, 'a name too long');
-- both checks, one written on the column and one on the table
insert into users (id, name, age) values (7, 'gil', -1);
insert into users (id, name, age) values (7, 'gil', 200);
insert into users (id, name, age) values (7, 'gil', 149);

-- a column with no value, no default and no NULL allowed
insert into users (id, email) values (8, 'h@x');
-- the same column named twice
insert into users (id, id, name) values (9, 9, 'ida');
-- a column that does not exist
insert into users (id, nope) values (9, 1);
-- and a positional insert still has to fill every column
insert into users values (9, 'i@x', 'ida');

-- UPDATE is held to the same promises
update users set age = -5 where id = 1;
update users set name = NULL where id = 1;
update users set id = 2 where id = 1;
update users set role = 'staff' where id = 1;
select id, name, role, age from users where id = 1;

-- one statement, every row: the collision is between the new rows themselves
update users set email = 'same@x';
-- and nothing was written before it was found
select count(*) from users where email = 'same@x';

-- deleting frees the value again
delete from users where id = 1;
insert into users values (1, 'a@x', 'ada', 'user', 1);
select id, email, role from users order by id;

-- the unique index survives a vacuum, which renumbers every row
vacuum users;
.indexes
insert into users values (2, 'z@x', 'zed', 'user', 1);
select count(*) from users;
