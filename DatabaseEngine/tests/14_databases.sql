.databases
create table t (id int, tag text)
insert into t values (1, 'in main')
create database shop
create database shop
create database blog
.databases
use shop
.databases
.tables
create table products (id int, name text)
insert into products values (1, 'widget')
insert into products values (2, 'gadget')
create index pidx on products (id)
select * from products order by id
select * from t
drop table t
use blog
.tables
create table t (id int, tag text)
insert into t values (99, 'in blog')
select * from t
use main
.tables
select * from t
use nope
use
create database
create database main
.databases
.save tmp_test.db
create database temp1
use temp1
create table gone (x int)
.databases
.load tmp_test.db
.databases
.tables
select * from t
use shop
.tables
.indexes
select * from products order by id
select * from products where id = 2
use blog
select * from t
.exit
