-- BEGIN suspends the per-statement commit; COMMIT ends the transaction.
-- Rolling back needs a file to roll back to, so the undo itself is checked by
-- tests/recovery.sh - what is pinned here is the state machine around it.
create table t (id int, name text);
insert into t values (1, 'one');

begin;
insert into t values (2, 'two');
insert into t values (3, 'three');
select count(*) from t;
commit;
select count(*) from t;

-- a transaction is not reentrant
begin;
begin;
commit;

-- and neither half works on its own
commit;
rollback;

-- an in-memory session has no file to go back to, so it cannot roll back
begin;
insert into t values (4, 'four');
rollback;
-- the statements stand, because nothing undid them
select count(*) from t;
commit;

-- errors inside a transaction are still errors, and leave it open
begin;
insert into t values (5);
insert into t values (5, 'five');
commit;
select count(*) from t;
