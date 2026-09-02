create database shop;
use shop;

create table users (id int, name text, city text, age int);
insert into users values (1, 'tomer', 'haifa', 30);
insert into users values (2, 'dana', 'tel aviv', 25);
insert into users values (3, 'yossi', 'haifa', 41);
insert into users values (4, 'maya', 'eilat', 25);
insert into users values (5, 'noa', null, null);
insert into users values (6, 'avi', 'tel aviv', 30);

create table orders (oid int, uid int, item text, total int);
insert into orders values (100, 1, 'book', 250);
insert into orders values (101, 1, 'pen', 90);
insert into orders values (102, 2, 'book', 400);
insert into orders values (103, 3, 'lamp', 120);
insert into orders values (104, 3, 'book', 60);
insert into orders values (105, 9, 'ghost', 5);

create table items (item text, price int);
insert into items values ('book', 30);
insert into items values ('pen', 5);
insert into items values ('lamp', 60);

.tables
.databases

select * from users;
select name, city from users;
select distinct city from users;

select name from users where age = 25;
select name from users where age != 25;
select name, age from users where age > 29;
select name, age from users where age >= 30;
select name, age from users where age < 30;
select name, age from users where age <= 25;
select name from users where city = 'haifa';

select name from users where city is null;
select name from users where city is not null;
select name from users where age > 20;

select name from users where name like 'a%';
select name from users where name like '%a';
select name from users where name like '%o%';
select name from users where name like '_o_';
select name from users where name not like '%a%';

select name from users where city = 'haifa' and age > 35;
select name from users where city = 'haifa' or city = 'eilat';
select name from users where not city = 'haifa';
select name from users where (city = 'haifa' or city = 'eilat') and age < 35;
select name from users where city = 'haifa' and (age = 30 or age = 41);

select city, count(*) from users group by city;
select city, count(*), min(age), max(age), sum(age) from users group by city;
select count(*) from users;
select min(name), max(name) from users;
select city, age, count(*) from users group by city, age;
select city, count(*) from users group by city having count(*) > 1;
select city, sum(age) from users group by city having sum(age) > 50;

select name, age from users order by age;
select name, age from users order by age desc;
select name, age from users order by age desc, name asc;
select city from users order by city;

select name from users order by name limit 3;
select distinct city from users order by city limit 2;
select city, count(*) from users group by city order by city limit 2;

create index users_city on users (city);
create index users_age on users (age);
create index orders_uid on orders (uid);
.indexes

.explain on
select name from users where city = 'haifa';
select name from users where age > 29;
select name from users where city like 'ha%';
select name from users where city = 'haifa' and age > 35;
select name from users where city = 'haifa' or age > 35;
select name from users where not city = 'haifa';
.explain off

select name, total from users, orders where users.id = orders.uid;
select name, total from users join orders on users.id = orders.uid;
select name, total from users inner join orders on users.id = orders.uid where total > 100;
select users.name, orders.item, price from users join orders on users.id = orders.uid join items on orders.item = items.item order by price desc, name;
select name, count(*), sum(total) from users join orders on id = uid group by name order by name;
select name, sum(total) from users join orders on id = uid group by name having sum(total) > 200;
select distinct city from users join orders on users.id = orders.uid;
select count(*) from users, orders;
select name from users join orders on users.id = orders.uid and total > 300;

update users set age = 26 where name = 'dana';
select name, age from users where name = 'dana';
update users set city = 'haifa', age = 31 where name = 'noa';
select * from users where name = 'noa';
update orders set total = 0 where uid = 9;
select oid, total from orders where uid = 9;

delete from orders where total = 0;
select count(*) from orders;
delete from users where age > 40;
select name from users order by name;

vacuum users;
select name from users where city = 'haifa' order by name;

.save demo_saved.db
drop index users_age;
drop table items;
.tables
.indexes
.load demo_saved.db
.tables
.indexes
select count(*) from items;

select nothing from users;
select * from nosuchtable;
insert into users values (1);
create table users (x int);
select id from users, orders;
select name from users join orders;
select * from users, users;
select name from users where age = 'thirty';
select name from users where age = null;
insert into users values (7, "quoted", 'x', 1);

use main;
.tables
drop database shop;
.databases
use shop;
