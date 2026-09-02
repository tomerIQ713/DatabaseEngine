-- comments run to end of line and leave no statement behind
create table t (id int, city text);   -- trailing comments work too
insert into t values (1, 'haifa');
insert into t values (2, 'haifa');
insert into t values (3, 'eilat');
insert into t values (4, null);
-- ORDER BY names output columns, aggregates included
select city, count(*) from t group by city order by count(*) desc;
select city, count(*) from t group by city order by count(*) asc, city;
select city, sum(id) from t group by city order by sum(id) desc, city;
select city, count(*) as n from t group by city order by n desc, city;
select city, min(id), max(id) from t group by city order by max(id);
select id from t where city = '-- not a comment';
select id from t order by id desc limit 2;
