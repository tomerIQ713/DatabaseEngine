#!/bin/sh
# Starts the engine as a server and checks that readers actually overlap.
#   ./tests/parallel.sh [path-to-exe] [port]
#
# Separate from wire.sh because it is a timing measurement rather than a
# protocol check, and because it is the one worth running under a thread
# sanitizer: several connections reading the same pages at the same time is
# exactly where a race would be.

db=${1:-x64/Debug/DatabaseEngine.exe}
port=${2:-5460}

if [ ! -x "$db" ]; then echo "engine not found: $db"; exit 2; fi
if ! command -v python3 > /dev/null 2>&1; then
    echo "python3 not found: skipping the parallel test"; exit 0
fi

dir=$(dirname "$0")

"$db" :memory: --port "$port" > "$dir/parallel_server.log" 2>&1 &
server=$!

# Give it a moment to bind before the first connection.
sleep 2

python3 "$dir/parallel.py" "$port" "${ROWS:-3000}"
result=$?

kill -9 "$server" > /dev/null 2>&1
wait "$server" 2>/dev/null

if [ "$result" != "0" ]; then
    echo "--- server log ---"
    cat "$dir/parallel_server.log"
fi

rm -f "$dir/parallel_server.log"
exit $result
