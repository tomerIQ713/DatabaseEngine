#!/bin/sh
# The PostgreSQL wire protocol cannot be checked with golden files: it is a
# conversation over a socket. This starts the engine as a server and runs
# tests/wire.py against it, which speaks the client half.
#
#   ./tests/wire.sh [path-to-exe] [port]
#
# Needs python3 for the client. With psql installed you can do the same thing
# by hand:  psql -h 127.0.0.1 -p 5433

db=${1:-x64/Debug/DatabaseEngine.exe}
port=${2:-5433}

if [ ! -x "$db" ]; then echo "engine not found: $db"; exit 2; fi
if ! command -v python3 > /dev/null 2>&1; then
    echo "python3 not found: skipping the wire test"; exit 0
fi

dir=$(dirname "$0")
work="$dir/wire_tmp"
rm -rf "$work"; mkdir -p "$work"

"$db" "$work/w.db" --port "$port" > "$work/server.log" 2>&1 &

# give it a moment to bind before the client knocks
sleep 2

python3 "$dir/wire.py" "$port"
result=$?

# the engine serves until it is killed; there is no quit over the wire
if command -v taskkill > /dev/null 2>&1; then
    taskkill //F //IM "$(basename "$db")" > /dev/null 2>&1
else
    pkill -9 -f "$(basename "$db")" > /dev/null 2>&1
fi

sleep 1
rm -rf "$work"
exit $result
