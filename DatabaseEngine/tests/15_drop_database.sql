create database shop
create database blog
use shop
create table products (id int, name text)
insert into products values (1, 'widget')
create index pidx on products (id)
use blog
create table posts (id int, title text)
insert into posts values (10, 'hello')
use main
create table t (id int)
insert into t values (1)
.databases
drop database blog
.databases
drop database blog
drop database nope
drop database main
drop database
use shop
.databases
drop database shop
.databases
.tables
select * from t
create database reused
use reused
.tables
.indexes
select * from products
create table products (other text)
insert into products values ('clean slot')
select * from products
.save tmp_test.db
create database temporary
.databases
.load tmp_test.db
.databases
.tables
select * from products
use main
select * from t
.exit
