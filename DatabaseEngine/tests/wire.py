"""Checks the PostgreSQL wire protocol by speaking it.

The protocol cannot be checked with golden files: it is a conversation over a
socket, not a transcript. This is the client half of what psql does for typed
SQL - SSLRequest, StartupMessage, Query, Terminate - and it asserts on what
comes back, so a change to the message framing fails here rather than silently
in someone's psql.

    python3 tests/wire.py [port]

Run through tests/wire.sh, which starts the engine first.
"""
import socket
import struct
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5433

passed = 0
failed = 0


def check(name, expected, actual):
    global passed, failed
    if expected == actual:
        passed += 1
    else:
        failed += 1
        print("FAIL %s: expected %r got %r" % (name, expected, actual))


def recv_exactly(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("server closed the connection")
        buf += chunk
    return buf


def read_message(sock):
    tag = recv_exactly(sock, 1).decode("latin-1")
    (length,) = struct.unpack("!I", recv_exactly(sock, 4))
    return tag, (recv_exactly(sock, length - 4) if length > 4 else b"")


def decode(tag, body):
    """One message, as the plain values a test wants to compare."""
    if tag == "T":
        (n,) = struct.unpack("!H", body[:2])
        at, fields = 2, []
        for _ in range(n):
            end = body.index(b"\0", at)
            name = body[at:end].decode()
            at = end + 1
            oid = struct.unpack("!IHIhih", body[at:at + 18])[2]
            at += 18
            fields.append((name, oid))
        return ("T", fields)
    if tag == "D":
        (n,) = struct.unpack("!H", body[:2])
        at, values = 2, []
        for _ in range(n):
            (size,) = struct.unpack("!i", body[at:at + 4])
            at += 4
            if size < 0:
                values.append(None)
            else:
                values.append(body[at:at + size].decode())
                at += size
        return ("D", values)
    if tag == "C":
        return ("C", body.split(b"\0")[0].decode())
    if tag == "E":
        fields = {f[:1].decode(): f[1:].decode() for f in body.split(b"\0") if f}
        return ("E", fields.get("M", ""))
    if tag == "Z":
        return ("Z", body.decode())
    return (tag, body)


def connect():
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=10)

    # psql asks for SSL before anything else; a server that does not answer
    # looks like a hang rather than a refusal
    sock.sendall(struct.pack("!II", 8, 80877103))
    check("SSLRequest is declined", b"N", recv_exactly(sock, 1))

    params = b"user\0tester\0database\0main\0\0"
    sock.sendall(struct.pack("!II", 8 + len(params), 196608) + params)

    settings = {}
    while True:
        tag, body = read_message(sock)
        if tag == "S":
            key, value = body.split(b"\0")[:2]
            settings[key.decode()] = value.decode()
        elif tag == "Z":
            break
    check("startup ends idle", ("Z", "I"), decode("Z", body))
    check("client_encoding is announced", "UTF8", settings.get("client_encoding"))
    return sock


def query(sock, sql):
    payload = sql.encode() + b"\0"
    sock.sendall(b"Q" + struct.pack("!I", 4 + len(payload)) + payload)

    out = []
    while True:
        tag, body = read_message(sock)
        out.append(decode(tag, body))
        if tag == "Z":
            return out


sock = connect()

check("create reports what it created",
      [("C", "CREATE TABLE"), ("Z", "I")],
      query(sock, "create table w (id int primary key, name varchar(8), "
                  "score float, day date)"))

check("insert reports a row count libpq can read",
      [("C", "INSERT 0 1"), ("Z", "I")],
      query(sock, "insert into w values (1, 'ada', 2.5, '2024-05-01')"))

query(sock, "insert into w (id, name) values (2, 'bo')")

check("every column arrives with its own type",
      ("T", [("id", 23), ("name", 25), ("score", 701), ("day", 1082)]),
      query(sock, "select * from w")[0])

check("rows carry values, and NULL is a length of -1",
      [("D", ["1", "ada", "2.5", "2024-05-01"]),
       ("D", ["2", "bo", None, None]),
       ("C", "SELECT 2"),
       ("Z", "I")],
      query(sock, "select * from w order by id")[1:])

check("an error is an ErrorResponse, not a dropped connection",
      [("E", "duplicate value in a unique column"), ("Z", "I")],
      query(sock, "insert into w values (1, 'dup', 1.0, '2024-01-01')"))

check("and the session carries on afterwards",
      [("C", "INSERT 0 1"), ("Z", "I")],
      query(sock, "insert into w values (3, 'cy', 1.0, '2024-06-01')"))

check("one message may carry several statements",
      [("C", "INSERT 0 1"), ("C", "INSERT 0 1"), ("C", "SELECT 5"), ("Z", "I")],
      [m for m in query(sock, "insert into w (id) values (4); "
                              "insert into w (id) values (5); "
                              "select id from w")
       if m[0] in ("C", "Z")])

query(sock, "insert into w (id, name) values (6, 'a;b')")
check("a semicolon inside a string is not a separator",
      [("D", ["a;b"]), ("C", "SELECT 1"), ("Z", "I")],
      query(sock, "select name from w where name = 'a;b'")[1:])

# A statement too long for the buffer must be refused, not shortened. A
# truncated statement can still parse, and the same query then answers
# differently depending on how much whitespace it was written with.
check("an over-long statement is refused, not truncated",
      [("E", "statement too long"), ("Z", "I")],
      query(sock, "select id from w where id > 1" + " " * 8200 + "and id < 3"))

check("and the session carries on",
      ("C", "SELECT 1"),
      query(sock, "select id from w where id = 1")[-2])

# the transaction status byte is what psql reads to know it is in one
check("BEGIN is reported in the status byte", ("Z", "T"), query(sock, "begin")[-1])
query(sock, "insert into w (id) values (99)")
check("ROLLBACK ends it", ("Z", "I"), query(sock, "rollback")[-1])
check("and takes its rows with it",
      ("D", ["6"]), query(sock, "select count(*) from w")[1])

# a session that drops mid-transaction must not leave one open for the next
query(sock, "begin")
query(sock, "insert into w (id) values (98)")
sock.close()

sock = connect()
check("an abandoned transaction is rolled back at disconnect",
      ("D", ["6"]), query(sock, "select count(*) from w")[1])
check("and the next session starts idle",
      ("Z", "I"), query(sock, "select id from w limit 1")[-1])


# --- the extended query protocol -------------------------------------------
# Parse / Bind / Describe / Execute / Sync is how every driver that is not psql
# talks, and parameters are the reason it exists.

def message(tag, body):
    return tag + struct.pack("!I", 4 + len(body)) + body


def extended(sql, params=(), oids=(), limit=0, name=""):
    body = name.encode() + b"\0" + sql.encode() + b"\0"
    body += struct.pack("!H", len(oids)) + b"".join(struct.pack("!I", o) for o in oids)
    out = message(b"P", body)

    bind = b"\0" + name.encode() + b"\0" + struct.pack("!H", 0)
    bind += struct.pack("!H", len(params))
    for value in params:
        if value is None:
            bind += struct.pack("!i", -1)
        else:
            raw = str(value).encode()
            bind += struct.pack("!i", len(raw)) + raw
    bind += struct.pack("!H", 0)

    out += message(b"B", bind)
    out += message(b"D", b"P\0")
    out += message(b"E", b"\0" + struct.pack("!i", limit))
    out += message(b"S", b"")
    sock.sendall(out)

    seen = []
    while True:
        tag, body = read_message(sock)
        seen.append(decode(tag, body))
        if tag == "Z":
            return seen


check("Parse and Bind are acknowledged before anything runs",
      [("1", b""), ("2", b""), ("n", b""), ("C", "CREATE TABLE"), ("Z", "I")],
      extended("create table x (id int, name text, score float)"))

extended("insert into x values ($1, $2, $3)", (1, "ada", 2.5), (23, 25, 701))
extended("insert into x values ($1, $2, $3)", (2, "bo", 10.0), (23, 25, 701))
extended("insert into x values ($1, $2, $3)", (3, None, None), (23, 25, 701))

check("a parameter carries its type into the row",
      [("T", [("id", 23), ("name", 25), ("score", 701)]),
       ("D", ["1", "ada", "2.5"]),
       ("D", ["2", "bo", "10"]),
       ("C", "SELECT 2"),
       ("Z", "I")],
      extended("select id, name, score from x where id < $1 order by id",
               (3,), (23,))[2:])

check("a NULL parameter is a NULL, not the word",
      [("D", ["3"]), ("C", "SELECT 1"), ("Z", "I")],
      extended("select id from x where name is null", ())[3:])

check("text that looks like a number stays text",
      [("C", "SELECT 0"), ("Z", "I")],
      extended("select id from x where name = $1", ("123",), (25,))[3:])

check("a row limit suspends the portal instead of finishing it",
      [("D", ["1"]), ("D", ["2"]), ("s", b""), ("Z", "I")],
      extended("select id from x order by id", (), (), limit=2)[3:])

# an error inside a batch must skip to Sync and leave the session usable
sock.sendall(message(b"P", b"\0select nope from x\0" + struct.pack("!H", 0))
             + message(b"B", b"\0\0" + struct.pack("!HHH", 0, 0, 0))
             + message(b"E", b"\0" + struct.pack("!i", 0))
             + message(b"S", b""))
batch = []
while True:
    tag, body = read_message(sock)
    batch.append(decode(tag, body))
    if tag == "Z":
        break
check("a result with no rows still knows its own types",
      ("T", [("id", 23), ("name", 25), ("score", 701)]),
      extended("select id, name, score from x where id = $1", (99,), (23,))[2])

check("and describing a statement answers the shape it will return",
      ("T", [("id", 23), ("score", 701)]),
      extended("select id, score from x where name = $1", ("nobody",), (25,))[2])

check("an error skips the rest of the batch, up to Sync",
      [("1", b""), ("2", b""), ("E", "no such column"), ("Z", "I")], batch)

check("and the session carries on after it",
      [("D", ["3"]), ("C", "SELECT 1"), ("Z", "I")],
      extended("select count(*) from x")[3:])

sock.sendall(b"X" + struct.pack("!I", 4))
sock.close()

# ---------------------------------------------------------------------------
# Several connections at once.
#
# The engine runs one statement at a time, but a connection pool needs all of
# its connections to be open at once - which is the thing that did not work
# before sessions existed.
# ---------------------------------------------------------------------------
import threading

many = [connect() for _ in range(4)]

check("four connections are open at the same time", 4, len(many))

query(many[0], "create table conc (id int, who text)")
for i, one in enumerate(many):
    query(one, "insert into conc values (%d, 'c%d')" % (i, i))

check("each connection sees what the others committed",
      [[("D", ["4"])]] * 4,
      [[m for m in query(one, "select count(*) from conc") if m[0] == "D"]
       for one in many])

# Each session remembers its own current database, so USE on one does not move
# the others.
query(many[0], "create database sideways")
query(many[0], "use sideways")
query(many[0], "create table only_here (x int)")

check("USE moves only the session that ran it",
      ("C", "SELECT 1"),
      [m for m in query(many[0], "select count(*) from only_here")
       if m[0] == "C"][0])

check("and the others are still where they were",
      [("D", ["4"])],
      [m for m in query(many[1], "select count(*) from conc") if m[0] == "D"])

check("a table in another database is not visible from here",
      "no such table",
      [m[1] for m in query(many[0], "select * from conc") if m[0] == "E"][0])

query(many[0], "use main")

# A transaction holds the engine: another session's statement waits for the
# COMMIT rather than running inside the transaction or being refused.
#
# What is asserted is that the reader *waited* and that it saw the committed
# row. Not the order the two clients hear back in: the lock is released when
# ProcessStatement returns, which is before the committing client's own
# acknowledgement has been written to its socket, so the reader's answer can
# legitimately arrive first. Asserting that interleaving failed about half the
# time and was testing the wrong thing.
HELD = 1.5
waited = {}


def holder():
    query(many[0], "begin")
    query(many[0], "insert into conc values (900, 'tx')")
    time.sleep(HELD)
    query(many[0], "commit")


def waiter():
    time.sleep(0.4)                       # once the transaction is under way
    start = time.time()
    rows = [m for m in query(many[2], "select count(*) from conc") if m[0] == "D"]
    waited["seconds"] = time.time() - start
    waited["rows"] = rows


first = threading.Thread(target=holder)
second = threading.Thread(target=waiter)
first.start(); second.start()
first.join(); second.join()

# The holder sleeps HELD seconds with the transaction open and the reader asks
# 0.4s in, so a reader that was actually blocked waits about HELD - 0.4. Half
# of that is a wide enough margin to be about blocking rather than about timing.
check("a reader waits for an open transaction rather than running inside it",
      True, waited["seconds"] > (HELD - 0.4) / 2)

check("and then sees the committed row", [("D", ["5"])], waited["rows"])
for one in many:
    one.sendall(b"X" + struct.pack("!I", 4))
    one.close()

print("%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
