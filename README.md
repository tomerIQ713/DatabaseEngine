# DatabaseEngine

[![CI](https://github.com/tomerIQ713/DatabaseEngine/actions/workflows/ci.yml/badge.svg)](https://github.com/tomerIQ713/DatabaseEngine/actions/workflows/ci.yml)

A SQL database engine written from scratch in C99 — no dependencies, no
libraries, about 13,000 lines. It has a buffer pool, write-ahead logging with
crash recovery, persistent B+ tree indexes, checksummed pages, and it speaks
the PostgreSQL wire protocol, so the official `psql` client connects to it and
gets rows back.

```console
$ ./db shop.db
db> select city, count(*), avg(age) from users group by city order by count(*) desc;
city | count(*) | avg(age)
haifa | 2 | 35.5
tel aviv | 2 | 27.5
eilat | 1 | 25
(3 rows)
```

The same engine also serves the PostgreSQL wire protocol, so the official
client connects to it:

```bash
./db shop.db --port 5433
```

```bash
psql -h 127.0.0.1 -p 5433
```

Third-party drivers work too — the extended query protocol is implemented, so
`pg8000` and friends can prepare statements and bind parameters against it.

## Build

No dependencies beyond a C compiler.

```bash
cc -std=c17 -O2 -o db *.c
```

Builds on Linux, macOS and Windows; CI runs the suites on all three, plus a
pass under AddressSanitizer and UBSan. On Windows add `-lws2_32` for the
socket layer. There is also a Visual Studio
solution (`DatabaseEngine.slnx`) if you prefer the IDE.

```bash
./db :memory:        # throwaway session
./db shop.db         # opens, and rewrites on clean exit
./db shop.db --port 5433
```

## Architecture

It is structured as a compiler with an execution stage on the end. One
statement goes through the whole pipeline before the next one starts.

```
  SQL text
     │
     ▼
┌─────────────┐   TokenList    ┌─────────────┐   Statement    ┌─────────────┐
│    lexer    │───────────────▶│   parser    │───────────────▶│  semantic   │
│  03_lexer   │                │  04_parser  │  (rec. descent)│ 05_semantic │
└─────────────┘                └─────────────┘                └──────┬──────┘
                                                          validated  │
                                                                     ▼
┌─────────────┐   ResultSet    ┌─────────────────────────────────────────────┐
│   output    │◀───────────────│                  executor                   │
│ 08_result   │                │                 06_executor                 │
└─────────────┘                └─────────────────────────────────────────────┘
                                    │            │              │
                          ┌─────────┘            │              └─────────┐
                          ▼                      ▼                        ▼
                   ┌─────────────┐        ┌─────────────┐          ┌─────────────┐
                   │   catalog   │        │   storage   │          │   B+ trees  │
                   │ 02_catalog  │        │ 07_storage  │          │  11_index   │
                   └─────────────┘        └──────┬──────┘          └──────┬──────┘
                                                 │  slotted pages         │
                                                 └──────────┬─────────────┘
                                                            ▼
                                                    ┌─────────────┐
                                                    │ buffer pool │  CLOCK
                                                    │   14_pool   │  + checksums
                                                    └──────┬──────┘
                                                           │
                                        ┌──────────────────┴──────────────────┐
                                        ▼                                     ▼
                                 ┌─────────────┐                       ┌─────────────┐
                                 │  page file  │                       │  write-ahead│
                                 │ 12_persist  │                       │  log 15_wal │
                                 └─────────────┘                       └─────────────┘
```

`16_wire.c` is a second front door onto the same pipeline: a statement arriving
over a socket goes through `ProcessStatement` exactly as a typed one does.

## What's in it

**Storage.** Rows live in 8 KB slotted pages — a slot directory growing up from
the front, variable-length records growing down from the back. A row of two ints
costs about a dozen bytes. Pages are owned by a buffer pool with CLOCK
replacement, so a database larger than memory works: a 151-page database runs in
64 frames.

**Durability.** Every statement that changes anything ends in an fsync of the
write-ahead log, and only then reports success. Frames carry whole pages, which
costs log volume and buys idempotent replay — recovery needs no undo pass. A
crash mid-statement loses that statement whole and nothing else.
`BEGIN`/`COMMIT`/`ROLLBACK` exist to amortise that fsync over a bulk load;
rollback works because `BEGIN` checkpoints first, which makes the file its own
before-image.

**Integrity.** Every page carries a checksum over itself, verified on the way in
from disk — including the catalog pages, which are the ones most worth catching.
A page that fails becomes an error rather than an empty table.

**Indexes.** B+ trees whose nodes are pool pages, so a tree is saved and reopened
rather than rebuilt by scanning. Entry size follows the key type; float keys are
stored as order-preserving integers so one comparison serves every type.

**SQL.** Selection, projection, joins (hash join on an equality, nested loop
otherwise), `GROUP BY` with aggregates, `HAVING`, `ORDER BY` (top-N when there's
a `LIMIT`), `DISTINCT`, `LIKE`, arithmetic expressions, `NULL` with proper
tri-state logic, constraints (`PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `DEFAULT`,
`CHECK`), and the types `int`, `text`, `float`, `date`, `varchar(n)`.

**Wire protocol.** PostgreSQL v3, both the simple and the extended query
protocol.

## Measured

`tests/bench.sh` generates its own data and reproduces these. One run on a
Windows laptop, MinGW gcc `-O2`; they are wall clock over the whole session,
so the load is in there too — fine for differences of this size, and honest
about what is being timed.

| | |
|---|---|
| 500 point queries over 200,000 rows, full scan | 4.40 s |
| the same 500 with a B+ tree, *including building it* | **1.03 s** |
| 2,000 inserts, one fsync per statement | 6.11 s |
| the same 2,000 inside one transaction | **0.11 s** |
| hash join, 20,000 × 5,000 rows, plus loading both | 0.18 s |
| `order by` 50,000 rows with `limit 5`, plus loading them | 0.29 s |
| printing 50,000 rows | 0.28 s |

The last three are whole sessions rather than isolated operations, which is
why they look flat: the join itself is well under a millisecond, and loading
25,000 rows is most of what is being measured. That is the point of showing
them — the operations these replaced could not have hidden inside a session
this short. The nested loop this join replaced took 2.96 s on the same data.

Design decisions behind these, and the measurements taken while making them,
are in the design notes below.

## Tests

```bash
./tests/run.sh ./db          # 30 golden-file tests
./tests/recovery.sh ./db     # 16 crash-recovery tests (kills the process mid-session)
./tests/wire.sh ./db         # 30 protocol tests (needs python3)
./tests/bench.sh ./db        # the table above
```

Crash recovery and the wire protocol can't be checked with golden files — one
needs the process killed, the other is a conversation over a socket — so they
have their own scripts.

There is also a fuzzer, which mutates SQL and protocol messages and shrinks any
crash to the fewest statements that still cause it:

```bash
python3 tests/fuzz.py sql  --exe ./db --runs 300
python3 tests/fuzz.py wire --exe ./db --runs 300
```

## Design notes

[`CLAUDE.md`](DatabaseEngine/CLAUDE.md) is the long version: every invariant, why
each one exists, and what broke when it didn't hold. It is written for whoever
edits this next.

## License

MIT — see [LICENSE](LICENSE).
