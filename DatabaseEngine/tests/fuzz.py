"""Throws malformed input at the engine and reports anything that crashes it.

Two targets:

    python3 tests/fuzz.py sql  [--runs N] [--seed S] [--exe path]
    python3 tests/fuzz.py wire [--runs N] [--seed S] [--exe path] [--port P]

The SQL target feeds mutated statements through the ordinary stdin interface,
which is the whole reason there is no C harness here: the engine already reads
statements from a pipe, so the fuzzer is a generator and a subprocess.

The wire target sends deliberately broken protocol messages - lengths that lie,
bodies that stop early, types nobody defined - because those bytes reach a
parser before anything has authenticated.

A run is reproducible from its seed. A crash is shrunk to the fewest statements
that still cause it and written to tests/fuzz_crashes/.
"""
import os
import random
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CRASHES = os.path.join(HERE, "fuzz_crashes")

KEYWORDS = [
    "select", "from", "where", "group", "by", "order", "asc", "desc", "limit",
    "distinct", "having", "insert", "into", "values", "update", "set", "delete",
    "create", "table", "index", "drop", "database", "use", "vacuum", "join",
    "inner", "on", "as", "and", "or", "not", "is", "null", "like", "int",
    "text", "float", "date", "varchar", "primary", "key", "unique", "default",
    "check", "begin", "commit", "rollback", "count", "sum", "avg", "min", "max",
]

SEEDS = [
    "create table t (id int primary key, name varchar(8) not null, "
    "score float default 0.0, day date, n int check (n > 0))",
    "create index t_id on t (id)",
    "insert into t values (1, 'a', 1.5, '2024-01-01', 2)",
    "insert into t (id, name) values (2, 'b')",
    "select * from t",
    "select id, count(*), avg(score) from t group by id having count(*) > 0",
    "select t.id from t, t u where t.id = u.id",
    "select id from t where name like 'a%' and score >= 1.0 or id is not null",
    "select id from t order by id desc limit 3",
    "select id, n * 2, n + 1, n / 2, n % 3, -n from t",
    "select id, (n + 1) * (n - 1) from t where n * 2 > 4 order by n * 2",
    "select sum(n * 2), avg(n + score) from t group by id having sum(n * 2) > 0",
    "update t set n = n * 2 + 1 where n - 1 > 0",
    "select score * 1.5 from t where -n < 0",
    "update t set name = 'z' where id = 1",
    "delete from t where id = 1",
    "vacuum t",
    "begin", "commit", "rollback",
    "create database d", "use d", "drop database d",
    ".tables", ".indexes", ".pool", ".explain on", ".databases",
]

PUNCT = list("(),;*='<>!-+/%.\"\\ \t")


def mutate(rng, text):
    """One edit, chosen from the ways a statement usually goes wrong."""
    if not text:
        return rng.choice(SEEDS)

    pick = rng.randrange(14)

    if pick == 0:                                   # cut it short
        return text[:rng.randrange(len(text))]
    if pick == 1:                                   # drop a character
        at = rng.randrange(len(text))
        return text[:at] + text[at + 1:]
    if pick == 2:                                   # duplicate a stretch
        at = rng.randrange(len(text))
        return text[:at] + text[at:at + rng.randrange(1, 12)] + text[at:]
    if pick == 3:                                   # a stray punctuation mark
        at = rng.randrange(len(text) + 1)
        return text[:at] + rng.choice(PUNCT) + text[at:]
    if pick == 4:                                   # swap in another keyword
        words = text.split()
        if words:
            words[rng.randrange(len(words))] = rng.choice(KEYWORDS)
        return " ".join(words)
    if pick == 5:                                   # numbers at the edges
        return text + " " + rng.choice(
            ["2147483647", "-2147483648", "2147483648", "99999999999999999999",
             "0", "-0", "1.7976931348623157e308", "0.0", "-1.5"])
    if pick == 6:                                   # nesting
        depth = rng.randrange(1, 40)
        return "select id from t where " + "(" * depth + "id = 1" + ")" * depth
    if pick == 7:                                   # an unbalanced quote
        return text + " '" + "x" * rng.randrange(0, 40)
    if pick == 8:                                   # long identifier or literal
        return text.replace("t", "t" + "x" * rng.randrange(1, 200), 1)
    if pick == 9:                                   # a long condition chain
        n = rng.randrange(1, 40)
        return "select id from t where " + " and ".join(["id = 1"] * n)
    if pick == 10:                                  # many columns
        n = rng.randrange(1, 30)
        return "select " + ", ".join(["id"] * n) + " from t"

    if pick == 11:                                  # arithmetic where it fits
        at = rng.randrange(len(text) + 1)
        return (text[:at] + " " + rng.choice(list("+-*/%")) + " "
                + rng.choice(["1", "n", "0", "score", "(n)"]) + " " + text[at:])
    if pick == 12:                                  # a pile of unary minuses
        return ("select " + "-" * rng.randrange(1, 30) + "n from t"
                if rng.randrange(2) else
                "select id from t where n = " + "-" * rng.randrange(1, 30) + "1")

    at = rng.randrange(len(text))                   # flip a byte
    return text[:at] + chr(rng.randrange(32, 127)) + text[at + 1:]


def make_case(rng):
    lines = []
    for _ in range(rng.randrange(2, 14)):
        base = rng.choice(SEEDS)
        for _ in range(rng.randrange(0, 3)):
            base = mutate(rng, base)
        lines.append(base + (";" if not base.startswith(".") else ""))
    return lines


def run_sql(exe, lines, timeout=15):
    """Returns None if the engine survived, or a description of how it did not."""
    text = "\n".join(lines) + "\n.exit\n"
    try:
        done = subprocess.run([exe, ":memory:"], input=text.encode(),
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return "hung"

    # a clean refusal is a pass; only dying is a failure
    if done.returncode != 0:
        return "exit %d" % done.returncode
    return None


def shrink(exe, lines, verdict):
    """The fewest statements that still do it."""
    changed = True
    while changed and len(lines) > 1:
        changed = False
        for i in range(len(lines)):
            shorter = lines[:i] + lines[i + 1:]
            if shorter and run_sql(exe, shorter) == verdict:
                lines = shorter
                changed = True
                break
    return lines


def save(name, body):
    os.makedirs(CRASHES, exist_ok=True)
    path = os.path.join(CRASHES, name)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(body)
    return path


def fuzz_sql(exe, runs, seed):
    rng = random.Random(seed)
    found = 0

    for run in range(runs):
        lines = make_case(rng)
        verdict = run_sql(exe, lines)

        if verdict is None:
            continue

        found += 1
        small = shrink(exe, lines, verdict)
        path = save("sql_%d_%d.sql" % (seed, run), "\n".join(small) + "\n")
        print("CRASH (%s) after %d statement(s) -> %s" % (verdict, len(small), path))
        for line in small:
            print("    " + line)

    print("sql: %d run(s), %d crash(es), seed %d" % (runs, found, seed))
    return found


# ---------------------------------------------------------------- the wire ---

def broken_messages(rng):
    """Protocol messages that a client should never send, and one that is fine."""
    kind = rng.randrange(10)

    if kind == 0:                                   # a length that lies, huge
        return b"Q" + struct.pack("!I", 0x7FFFFFFF) + b"select 1"
    if kind == 1:                                   # a length below the header
        return b"Q" + struct.pack("!I", 1)
    if kind == 2:                                   # a type nobody defined
        return bytes([rng.randrange(128, 256)]) + struct.pack("!I", 8) + b"abcd"
    if kind == 3:                                   # a body that stops early
        return b"Q" + struct.pack("!I", 100) + b"select"
    if kind == 4:                                   # no terminator on the string
        body = b"select id from t" * rng.randrange(1, 4)
        return b"Q" + struct.pack("!I", 4 + len(body)) + body
    if kind == 5:                                   # an extended-protocol message
        return b"P" + struct.pack("!I", 12) + b"\0\0\0\0\0\0\0\0"
    if kind == 6:                                   # random bytes
        n = rng.randrange(1, 64)
        return bytes(rng.randrange(256) for _ in range(n))
    if kind == 7:                                   # a very long statement
        body = (b"select id from t where " + b" " * 9000 + b"id = 1\0")
        return b"Q" + struct.pack("!I", 4 + len(body)) + body
    if kind == 8:                                   # empty query
        return b"Q" + struct.pack("!I", 5) + b"\0"

    body = b"select 1 from t\0"                     # something valid, in between
    return b"Q" + struct.pack("!I", 4 + len(body)) + body


def wire_session(port, rng, messages):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    try:
        if rng.randrange(4):                        # usually a real handshake
            sock.sendall(struct.pack("!II", 8, 80877103))
            sock.recv(1)
            params = b"user\0f\0database\0main\0\0"
            sock.sendall(struct.pack("!II", 8 + len(params), 196608) + params)
            deadline = time.time() + 3
            while time.time() < deadline:
                head = sock.recv(5)
                if len(head) < 5:
                    break
                (length,) = struct.unpack("!I", head[1:5])
                if length > 4:
                    sock.recv(min(length - 4, 65536))
                if head[:1] == b"Z":
                    break
        else:                                       # or start mid-conversation
            sock.sendall(broken_messages(rng))

        for message in messages:
            sock.sendall(message)
            try:
                sock.recv(65536)
            except socket.timeout:
                pass
    finally:
        sock.close()


def fuzz_wire(exe, runs, seed, port):
    rng = random.Random(seed)
    work = os.path.join(HERE, "fuzz_tmp")
    os.makedirs(work, exist_ok=True)
    server = subprocess.Popen([exe, os.path.join(work, "f.db"), "--port", str(port)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    found = 0

    try:
        for run in range(runs):
            if server.poll() is not None:
                print("CRASH: the server died (exit %s) at run %d"
                      % (server.returncode, run))
                found += 1
                break

            messages = [broken_messages(rng)
                        for _ in range(rng.randrange(1, 6))]
            try:
                wire_session(port, rng, messages)
            except (socket.timeout, ConnectionError, OSError):
                pass                                # the server hanging up is fine

        time.sleep(0.5)
        if server.poll() is not None and found == 0:
            print("CRASH: the server died (exit %s)" % server.returncode)
            found += 1
        elif found == 0:
            # it must still answer after everything above
            try:
                wire_session(port, random.Random(0), [])
            except Exception as problem:             # noqa: BLE001
                print("CRASH: the server stopped answering: %s" % problem)
                found += 1
    finally:
        if server.poll() is None:
            server.kill()
        server.wait()

    print("wire: %d run(s), %d crash(es), seed %d" % (runs, found, seed))
    return found


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "sql"
    args = sys.argv[2:]

    def option(name, fallback):
        return type(fallback)(args[args.index(name) + 1]) if name in args else fallback

    exe = option("--exe", "./db.exe")
    runs = option("--runs", 200)
    seed = option("--seed", random.randrange(1 << 30))
    port = option("--port", 5455)

    if target == "wire":
        return 1 if fuzz_wire(exe, runs, seed, port) else 0
    return 1 if fuzz_sql(exe, runs, seed) else 0


if __name__ == "__main__":
    sys.exit(main())
