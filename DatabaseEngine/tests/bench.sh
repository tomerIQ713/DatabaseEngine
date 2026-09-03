#!/bin/sh
# Reproduces the numbers in the README.
#   ./tests/bench.sh [path-to-exe]
#
# Each case generates its own data, so this takes a minute or two. Times are
# wall clock over the whole session, which includes loading the input - fine
# for numbers that differ by orders of magnitude, and honest about it.

db=${1:-./db.exe}
if [ ! -x "$db" ]; then echo "engine not found: $db"; exit 2; fi

tmp=$(dirname "$0")/bench_tmp
rm -rf "$tmp"; mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT

# Times one session, printing seconds with three decimals.
run() {
    start=$(date +%s%N)
    "$db" "$1" < "$2" > "$tmp/out.txt" 2>&1
    end=$(date +%s%N)
    echo "$start $end" | awk '{ printf "%.3f", ($2 - $1) / 1000000000 }'
}

rows() { sed -n 's/^(\([0-9]*\) rows\{0,1\})$/\1/p' "$tmp/out.txt" | tail -1; }

echo "engine: $db"
echo

# ---------------------------------------------------------------- hash join
# 20,000 x 5,000 on an equality. The nested loop would consider 100,000,000
# pairs; the hash join looks at each left row once.
echo "== hash join, 20,000 x 5,000 =="
{
    echo "create table a (id int, v int);"
    echo "create table b (id int, w int);"
    echo "begin;"
    awk 'BEGIN { for (i = 1; i <= 20000; i++) printf "insert into a values (%d, %d);\n", i, i * 2 }'
    awk 'BEGIN { for (i = 1; i <= 5000;  i++) printf "insert into b values (%d, %d);\n", i, i * 3 }'
    echo "commit;"
    echo "select count(*) from a, b where a.id = b.id;"
} > "$tmp/join.sql"
t=$(run :memory: "$tmp/join.sql")
echo "  $t s total session (load + join), $(rows) row(s) of output"
echo

# ------------------------------------------------------------------- top-N
# ORDER BY with a LIMIT keeps the best k in a heap instead of sorting 50,000
# rows. Both forms must agree - the fast path is only correct if it does.
echo "== order by 50,000 rows, limit 5 =="
{
    echo "create table t (id int, v int);"
    echo "begin;"
    awk 'BEGIN { srand(7); for (i = 1; i <= 50000; i++) printf "insert into t values (%d, %d);\n", i, int(rand() * 1000000) }'
    echo "commit;"
    echo "select id, v from t order by v desc limit 5;"
} > "$tmp/topn.sql"
t=$(run :memory: "$tmp/topn.sql")
echo "  $t s total session (load + top-N)"
echo

# --------------------------------------------------------------- print rate
# 50,000 rows out through the hand-formatted buffer rather than printf.
echo "== printing 50,000 rows =="
{
    echo "create table p (id int, v int);"
    echo "begin;"
    awk 'BEGIN { for (i = 1; i <= 50000; i++) printf "insert into p values (%d, %d);\n", i, i }'
    echo "commit;"
    echo "select * from p;"
} > "$tmp/print.sql"
t=$(run :memory: "$tmp/print.sql")
echo "  $t s total session (load + print), $(rows) row(s)"
echo

# ------------------------------------------------------------ index vs scan
# 500 point queries, so the measurement is the lookups rather than the load.
# The build is included in the indexed run and still wins by a wide margin.
echo "== 500 point queries over 200,000 rows =="
{
    echo "create table big (id int, v int);"
    echo "begin;"
    awk 'BEGIN { for (i = 1; i <= 200000; i++) printf "insert into big values (%d, %d);\n", i, i }'
    echo "commit;"
} > "$tmp/load.sql"
awk 'BEGIN { srand(3); for (q = 1; q <= 500; q++) printf "select v from big where id = %d;\n", int(rand() * 200000) + 1 }' > "$tmp/queries.sql"

cat "$tmp/load.sql" "$tmp/queries.sql" > "$tmp/scan.sql"
t=$(run :memory: "$tmp/scan.sql")
echo "  $t s without an index (500 full scans)"

{ cat "$tmp/load.sql"; echo "create index bi on big (id);"; cat "$tmp/queries.sql"; } > "$tmp/idx.sql"
t=$(run :memory: "$tmp/idx.sql")
echo "  $t s with one, build included"
echo

# ---------------------------------------------------------------- durability
# The cost of one fsync per statement, and what BEGIN saves.
echo "== 2,000 inserts to disk =="
{
    echo "create table d (id int, v int);"
    awk 'BEGIN { for (i = 1; i <= 2000; i++) printf "insert into d values (%d, %d);\n", i, i }'
    echo ".exit"
} > "$tmp/nosync.sql"
rm -f "$tmp/a.db"
t=$(run "$tmp/a.db" "$tmp/nosync.sql")
echo "  $t s one statement at a time (one fsync each)"

{
    echo "create table d (id int, v int);"
    echo "begin;"
    awk 'BEGIN { for (i = 1; i <= 2000; i++) printf "insert into d values (%d, %d);\n", i, i }'
    echo "commit;"
    echo ".exit"
} > "$tmp/sync.sql"
rm -f "$tmp/b.db"
t=$(run "$tmp/b.db" "$tmp/sync.sql")
echo "  $t s inside one transaction (one fsync total)"
