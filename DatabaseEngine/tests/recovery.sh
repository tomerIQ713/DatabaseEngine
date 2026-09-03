#!/bin/sh
# Crash recovery cannot be checked with golden files: it needs a process that
# dies without running its exit path. This kills the engine mid-session and
# checks that what it had committed is still there when it reopens.
#
#   ./tests/recovery.sh [path-to-exe]

db=${1:-x64/Debug/DatabaseEngine.exe}
if [ ! -x "$db" ]; then echo "engine not found: $db"; exit 2; fi

work=$(dirname "$0")/recovery_tmp
rm -rf "$work"; mkdir -p "$work"
file="$work/r.db"

pass=0
fail=0

check() {                                   # check <name> <expected> <actual>
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $1: expected [$2] got [$3]"
    fi
}

# Kills the engine started by the previous background pipeline, by the pid the
# shell recorded for it.
#
# Not by name: taskkill matches an exact image name, but pkill -f matches a
# substring of every command line on the machine, and "db" appears in this
# script's own. That killed the test run itself on Linux - exit 137 - while
# looking perfectly fine on Windows.
kill_engine() {
    [ -n "$engine" ] || return 0

    kill -9 "$engine" > /dev/null 2>&1
    wait "$engine" 2>/dev/null
    engine=""
    sleep 1
}

# --- commit some statements, then die without exiting ------------------------
{
    printf 'create table t (id int, name text);\n'
    printf "insert into t values (1, 'alpha');\n"
    printf "insert into t values (2, 'beta');\n"
    printf 'create index t_id on t (id);\n'
    printf "insert into t values (3, 'gamma');\n"
    sleep 10
} | "$db" "$file" > /dev/null 2>&1 &
engine=$!

sleep 3
[ -f "$file-wal" ] && wal=yes || wal=no
check "a log exists while the session is live" "yes" "$wal"
kill_engine

[ -f "$file-wal" ] && left=yes || left=no
check "the log outlives the crash" "yes" "$left"

# --- reopen: everything committed must be back -------------------------------
rows=$(printf 'select count(*) from t;\n.exit\n' | "$db" "$file" 2>&1 \
       | tr -d '\r' | sed -n '/count(.*)/{n;p;}')
check "rows survive the crash" "3" "$rows"

name=$(printf "select name from t where id = 2;\n.exit\n" | "$db" "$file" 2>&1 \
       | tr -d '\r' | sed -n '/db> name$/{n;p;}')
check "the index still answers" "beta" "$name"

[ -f "$file-wal" ] && after=yes || after=no
check "a clean exit folds the log away" "no" "$after"

# --- a half-written log must not be half-applied -----------------------------
{
    printf "insert into t values (4, 'delta');\n"
    printf "insert into t values (5, 'epsilon');\n"
    sleep 10
} | "$db" "$file" > /dev/null 2>&1 &
engine=$!

sleep 3
kill_engine

if [ -f "$file-wal" ]; then                 # tear the final frame in half
    size=$(wc -c < "$file-wal")
    dd if="$file-wal" of="$file-wal.cut" bs=1 count=$((size - 4108)) 2>/dev/null
    mv "$file-wal.cut" "$file-wal"
fi

rows=$(printf 'select count(*) from t;\n.exit\n' | "$db" "$file" 2>&1 \
       | tr -d '\r' | sed -n '/count(.*)/{n;p;}')
check "a torn commit is discarded whole, not half applied" "4" "$rows"

# --- the log and the file have to stay pointed at each other -----------------
# Each of these lost data once: a save marked pages clean that were only in
# some other file, a save over the open file handed the pool a read-only
# handle it still thought it could write, a load left the log describing the
# database it had just closed, and a log outlived the file it was written for.

count() {                                   # count <file>
    printf 'select count(*) from t;\n.exit\n' | "$db" "$1" 2>&1 \
        | tr -d '\r' | sed -n '/count(.*)/{n;p;}'
}

live="$work/live.db"
{
    printf 'create table t (id int);\n'
    printf 'insert into t values (1);\n'
    printf 'insert into t values (2);\n'
    printf ".save $work/copy.db\n"
    printf 'create table other (id int);\n'   # only new pages from here on
    printf 'insert into other values (1);\n'
    printf '.exit\n'
} | "$db" "$live" > /dev/null 2>&1
check "a save elsewhere does not strand the pages of this database" "2" "$(count "$live")"

same="$work/same.db"
{
    printf 'create table t (id int);\n'
    printf 'insert into t values (1);\n'
    printf ".save $same\n"                    # replaces the file being paged from
    printf 'insert into t values (2);\n'
    printf 'insert into t values (3);\n'
    printf '.exit\n'
} | "$db" "$same" > /dev/null 2>&1
check "a save over the open file leaves it writable" "3" "$(count "$same")"

first="$work/first.db"
second="$work/second.db"
printf 'create table t (id int);\ninsert into t values (1);\n.exit\n' \
    | "$db" "$second" > /dev/null 2>&1
{
    printf 'create table t (id int);\n'
    printf 'insert into t values (1);\n'
    printf ".load $second\n"
    printf 'insert into t values (2);\n'
    printf '.exit\n'
} | "$db" "$first" > /dev/null 2>&1
check "a load moves the log to the database it loaded" "2" "$(count "$second")"
check "and leaves the one it closed alone"            "1" "$(count "$first")"

# a log left by a crash must not be replayed over a file written since
gone="$work/gone.db"
{
    printf 'create table t (id int);\n'
    printf 'insert into t values (1);\n'
    sleep 10
} | "$db" "$gone" > /dev/null 2>&1 &
engine=$!
sleep 3
kill_engine
{
    printf 'create table t (id int);\n'
    printf 'insert into t values (1);\n'
    printf 'insert into t values (2);\n'
    printf 'insert into t values (3);\n'
    printf ".save $gone\n"
    printf '.exit\n'
} | "$db" ":memory:" > /dev/null 2>&1
check "a whole file written over a stale log wins" "3" "$(count "$gone")"

# --- transactions ------------------------------------------------------------
# What BEGIN suspends is durability, so what it is holding has to be the thing a
# crash loses - and what COMMIT wrote has to be the thing a crash keeps.
open_txn="$work/open.db"
{
    printf 'create table t (id int);\n'
    printf 'insert into t values (1);\n'
    printf 'begin;\n'
    printf 'insert into t values (2);\n'
    printf 'insert into t values (3);\n'
    sleep 10
} | "$db" "$open_txn" > /dev/null 2>&1 &
engine=$!
sleep 3
kill_engine
check "a crash inside a transaction loses it whole" "1" "$(count "$open_txn")"

closed_txn="$work/closed.db"
{
    printf 'create table t (id int);\n'
    printf 'begin;\n'
    printf 'insert into t values (1);\n'
    printf 'insert into t values (2);\n'
    printf 'commit;\n'
    printf 'insert into t values (3);\n'
    sleep 10
} | "$db" "$closed_txn" > /dev/null 2>&1 &
engine=$!
sleep 3
kill_engine
check "a committed transaction survives one" "3" "$(count "$closed_txn")"

# rollback puts the session back where BEGIN found it, in memory as well as on
# disk - the reopen below is what proves the file agrees with what it printed
rolled="$work/rolled.db"
{
    printf 'create table t (id int);\n'
    printf 'insert into t values (1);\n'
    printf 'begin;\n'
    printf 'insert into t values (2);\n'
    printf 'delete from t where id = 1;\n'
    printf 'rollback;\n'
    printf 'insert into t values (9);\n'
    printf '.exit\n'
} | "$db" "$rolled" > /dev/null 2>&1
check "rollback undoes the statements since BEGIN" "2" "$(count "$rolled")"

# --- a damaged page ----------------------------------------------------------
# Every page carries a checksum over itself, so a page that comes back changed
# is refused rather than served. The alternative is answering a query with
# whatever the corruption turned the rows into.
bad="$work/bad.db"
{
    printf 'create table t (id int, note text);\n'
    printf "insert into t values (1, 'alpha');\n"
    printf "insert into t values (2, 'beta');\n"
    printf '.exit\n'
} | "$db" "$bad" > /dev/null 2>&1

# page 0 is the catalog and page 1 the rows; the header page comes first, so
# page 1 starts two pages in
printf 'ZZZZZZZZ' | dd of="$bad" bs=1 seek=20384 conv=notrunc > /dev/null 2>&1

damaged=$(printf 'select * from t;\n.exit\n' | "$db" "$bad" 2>/dev/null \
          | tr -d '\r' | grep -c 'error 604')
check "a page that fails its checksum is refused" "1" "$damaged"

served=$(printf 'select * from t;\n.exit\n' | "$db" "$bad" 2>/dev/null \
         | tr -d '\r' | grep -c 'alpha')
check "and its rows are not handed out" "0" "$served"

# --- a schema change with no rows in it is still committed -------------------
# CREATE TABLE on its own dirties no page, so a commit that only looks for
# dirty pages finds nothing to do and the table lives only in memory. It then
# vanishes at the next ROLLBACK, and vanishes in a crash after the statement
# has already reported success - which is the promise this engine exists to
# keep.
empty="$work/e.db"
printf 'create table seed (i int);\ninsert into seed values (1);\n.exit\n' \
    | "$db" "$empty" > /dev/null 2>&1

kept=$(printf 'create table lonely (i int);\nbegin;\nrollback;\n.tables\n.exit\n' \
       | "$db" "$empty" 2>&1 | tr -d '\r' | grep -c '^lonely(')
check "a table with no rows survives a rollback" "1" "$kept"

# and the same thing with the engine killed instead of asked to roll back.
#
# CREATE TABLE must be the only statement in this session. Anything that
# touches a page leaves the pool dirty, and the next commit then writes the
# whole catalog - carrying the table along with it and hiding the bug.
crashed="$work/x.db"
printf 'create table seed (i int);\ninsert into seed values (1);\n.exit\n' \
    | "$db" "$crashed" > /dev/null 2>&1

{
    printf 'create table ghost (i int);\n'
    sleep 10
} | "$db" "$crashed" > /dev/null 2>&1 &
engine=$!

sleep 3
kill_engine

survived=$(printf '.tables\n.exit\n' | "$db" "$crashed" 2>&1 \
           | tr -d '\r' | grep -c '^ghost(')
check "and survives a crash, having reported success" "1" "$survived"

# --- a schema change is transactional too ------------------------------------
# ALTER changes the catalog in memory as well as on the page, and ROLLBACK puts
# it back by re-reading the catalog - the same reload that restores the rows.
schema="$work/s.db"
"$db" "$schema" > /dev/null 2>&1 <<'SQL'
create table t (a int, b text);
insert into t values (1, 'x');
.exit
SQL

# .tables prints one line per table, so the catalog is read straight off it
rolled=$(printf 'begin;\nalter table t add column c int default 9;\nalter table t rename to renamed;\nrollback;\n.tables\n.exit\n' \
         | "$db" "$schema" 2>&1 | tr -d '\r' | sed -n '/^t(/p')
check "rollback undoes a schema change" "t(a int, b text)" "$rolled"

printf 'begin;\nalter table t add column c int default 9;\ncommit;\n.exit\n' \
    | "$db" "$schema" > /dev/null 2>&1

kept=$(printf 'select * from t;\n.exit\n' | "$db" "$schema" 2>&1 \
       | tr -d '\r' | sed -n '/^db> a | b | c$/{n;p;}')
check "and a committed one survives" "1 | x | 9" "$kept"

rm -rf "$work"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
