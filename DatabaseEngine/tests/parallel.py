"""Checks that readers actually run at the same time.

This is a wall-clock measurement rather than an assertion about the code: the
same slow SELECT is run N times one after another on one connection, then N
times at once on N connections. If the engine lock is shared by readers the
second number is well below the first; if readers were still serialised the
two would match.

Under ThreadSanitizer the timings mean nothing - everything is slow - so the
speedup check is skipped there and the point of the run is whether TSan says
anything. The concurrent reads still happen either way, which is what has to
be watched.

    python3 tests/parallel.py [port] [rows]

Run through tests/parallel.sh, which starts the engine first.
"""
import os
import socket
import struct
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5460
ROWS = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
READERS = 4

# TSan slows everything down by more than the speedup being measured, so the
# timing half of this test is meaningless under it.
INSTRUMENTED = os.environ.get("TSAN_OPTIONS") or os.environ.get("SANITIZED")


def connect():
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=300)

    sock.sendall(struct.pack("!II", 8, 80877103))
    sock.recv(1)                                # SSL declined

    params = b"user\0tester\0database\0main\0\0"
    sock.sendall(struct.pack("!II", 8 + len(params), 196608) + params)

    while True:
        tag = sock.recv(1)
        (length,) = struct.unpack("!I", sock.recv(4))
        need = length - 4
        while need > 0:
            need -= len(sock.recv(need))
        if tag == b"Z":
            return sock


def query(sock, sql):
    payload = sql.encode() + b"\0"
    sock.sendall(b"Q" + struct.pack("!I", 4 + len(payload)) + payload)

    while True:
        tag = sock.recv(1)
        if not tag:
            raise EOFError("server closed the connection")
        (length,) = struct.unpack("!I", sock.recv(4))
        need = length - 4
        while need > 0:
            need -= len(sock.recv(need))
        if tag == b"Z":
            return


# ---------------------------------------------------------------------------
# An abandoned transaction must not outlive the connection that opened it.
#
# This suite serves :memory:, which is the case wire.sh cannot cover: it has no
# file to roll back to, so ROLLBACK there is an error. The transaction still
# has to end, or the next connection inherits one it never began - reporting
# itself inside a transaction, never committing, and holding the write lock
# against everyone else.
# ---------------------------------------------------------------------------
def status_byte(sock, sql):
    payload = sql.encode() + b"\0"
    sock.sendall(b"Q" + struct.pack("!I", 4 + len(payload)) + payload)

    while True:
        tag = sock.recv(1)
        (length,) = struct.unpack("!I", sock.recv(4))
        body, need = b"", length - 4
        while need > 0:
            chunk = sock.recv(need)
            need -= len(chunk)
            body += chunk
        if tag == b"Z":
            return body.decode()


abandoned = connect()
query(abandoned, "create table dropped (i int)")
query(abandoned, "begin")
query(abandoned, "insert into dropped values (1)")
abandoned.close()                               # gone, still inside it

time.sleep(1)

after = connect()
byte = status_byte(after, "select count(*) from dropped")
after.close()

if byte != "I":
    print("FAIL the next session inherited a transaction (status %r, wanted 'I')"
          % byte)
    sys.exit(1)

print("an abandoned transaction does not outlive its connection")

setup = connect()
query(setup, "create table big (a int, b int, c text)")
query(setup, "begin")                           # one fsync, not ROWS of them
for i in range(ROWS):
    query(setup, "insert into big values (%d, %d, 'row %d')" % (i, i * 7 % 1000, i))
query(setup, "commit")

# A nested-loop join with no equality to hash on: CPU-bound, and long enough
# that overlapping it is visible.
SLOW = ("select count(*) from big a, big b "
        "where a.a < 60 and b.a < 60 and a.b <> b.b")

query(setup, SLOW)                              # warm the pool

start = time.time()
for _ in range(READERS):
    query(setup, SLOW)
serial = time.time() - start

readers = [connect() for _ in range(READERS)]
for reader in readers:
    query(reader, "select count(*) from big")   # warm each thread

ready = threading.Barrier(READERS)
failures = []


def run(sock):
    try:
        ready.wait()
        query(sock, SLOW)
    except Exception as exc:                    # noqa: BLE001 - reported below
        failures.append(repr(exc))


threads = [threading.Thread(target=run, args=(sock,)) for sock in readers]

start = time.time()
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
parallel = time.time() - start

for sock in readers:
    sock.close()
setup.close()

if failures:
    print("FAIL a concurrent reader raised: %s" % failures[0])
    sys.exit(1)

print("%d readers serial %.2fs, together %.2fs" % (READERS, serial, parallel))

if INSTRUMENTED:
    print("instrumented build: timings not checked, %d readers ran together"
          % READERS)
    sys.exit(0)

speedup = serial / parallel if parallel > 0 else 0

if speedup < 1.5:
    print("FAIL readers did not overlap (%.2fx)" % speedup)
    sys.exit(1)

print("readers overlap, %.2fx" % speedup)
