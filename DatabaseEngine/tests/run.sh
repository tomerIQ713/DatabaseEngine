#!/bin/sh
# Runs every tests/*.sql through the engine and diffs against its .expected file.
#   ./tests/run.sh [path-to-exe]
# Regenerate baselines after an intentional change (read the diff first):
#   ./tests/run.sh --bless [path-to-exe]
#
# Output is stripped of CR so Windows console line endings do not show up as
# differences; baselines stay plain LF text.

bless=0
if [ "$1" = "--bless" ]; then bless=1; shift; fi

db=${1:-x64/Debug/DatabaseEngine.exe}
if [ ! -x "$db" ]; then echo "engine not found: $db"; exit 2; fi

dir=$(dirname "$0")
pass=0
fail=0

for sql in "$dir"/*.sql; do
    expected="${sql%.sql}.expected"
    actual=$("$db" :memory: < "$sql" 2>&1 | tr -d '\r')

    if [ "$bless" = "1" ]; then
        printf '%s\n' "$actual" > "$expected"
        echo "blessed $(basename "$sql")"
        continue
    fi

    if [ ! -f "$expected" ]; then
        echo "MISSING baseline: $expected"
        fail=$((fail + 1))
    elif [ "$actual" = "$(cat "$expected")" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $(basename "$sql")"
        printf '%s\n' "$actual" | diff - "$expected" | head -30
    fi
done

rm -f tmp_test.db          # scratch file written by the persistence test

[ "$bless" = "1" ] && exit 0

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
