# DatabaseEngine

A SQL database engine written from scratch in C99 — no dependencies, no
libraries, about 14,700 lines. It has a buffer pool, write-ahead logging with
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

No dependencies beyond a C compiler and the platform's threads.

```bash
cc -std=c17 -O2 -o db *.c -lpthread
```

On Windows, `-lws2_32` instead of `-lpthread`. There is also a Visual Studio
solution (`DatabaseEngine.slnx`) if you prefer the IDE.

Builds on Linux, macOS and Windows; CI runs the suites on all three, plus
passes under AddressSanitizer, UndefinedBehaviourSanitizer and
ThreadSanitizer.

```bash
./db :memory:        # throwaway session
./db shop.db         # opens, and rewrites on clean exit
./db shop.db --port 5433
```

There is a worked example in the repository — two tables, a join, grouping,
indexes and a second database:

```bash
./db :memory: < demo.sql
```

## Architecture

It is structured as a compiler with an execution stage on the end. A statement
goes through every stage before it produces a row; several statements can be
in the pipeline at once, one per connection, and the engine lock decides which
of them may run together.

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

**SQL.** Selection, projection, `INNER` and `LEFT OUTER JOIN` (hash join on an
equality, nested loop otherwise), subqueries (`IN`, `NOT IN`, `EXISTS`, and a
scalar on the right of an operator), `GROUP BY` with aggregates, `HAVING`,
`ORDER BY` (top-N when there's a `LIMIT`), `DISTINCT`, `LIKE`, arithmetic
expressions, `NULL` with proper tri-state logic, constraints (`PRIMARY KEY`,
`UNIQUE`, `NOT NULL`, `DEFAULT`, `CHECK`), `ALTER TABLE`, `VACUUM`, and the
types `int`, `text`, `float`, `date`, `varchar(n)`.

The NULL semantics are the real ones, not an approximation: `x NOT IN (a set
containing NULL)` correctly returns nothing, `count(col)` over an outer join's
unmatched row correctly counts zero, and `ON` and `WHERE` on an outer join mean
different things.

**Concurrency.** A thread per connection, up to 16, with one reader-writer lock
over the engine: `SELECT`s take it shared and genuinely run at the same time,
anything that writes takes it exclusive. Measured at **2.4x on 4 readers**.
Everything a statement scratches in — the text arena, the join heap, the
group table, the sort buffers — is thread-local, which is what made a
2,900-line executor safe to run concurrently without threading a context
through it. A transaction holds the write lock between statements, and is
rolled back if the client goes quiet while holding it.

**Wire protocol.** PostgreSQL v3, both the simple and the extended query
protocol. Each connection has its own prepared statements, portal and current
database.

## Measured

`tests/bench.sh` generates its own data and reproduces these. Wall clock over
a whole session, so the load is in there too — fine for differences of this
size, and honest about what is being timed. The absolute numbers move by a
third or so between runs on a laptop; the ratios are the point.

| | |
|---|---|
| 500 point queries over 200,000 rows, full scan | 2.60 s |
| the same 500 with a B+ tree, *including building it* | **0.95 s** |
| 2,000 inserts, one fsync per statement | 4.86 s |
| the same 2,000 inside one transaction | **0.12 s** |
| 4 concurrent readers, run one after another | 1.89 s |
| the same 4 at the same time | **0.65 s** |
| hash join, 20,000 × 5,000 rows, plus loading both | 0.22 s |
| `order by` 50,000 rows with `limit 5`, plus loading them | 0.28 s |
| printing 50,000 rows | 0.34 s |

The last three are whole sessions rather than isolated operations, which is
why they look flat: the join itself is well under a millisecond, and loading
25,000 rows is most of what is being measured. That is the point of showing
them — the operations these replaced could not have hidden inside a session
this short. The nested loop this join replaced took 2.96 s on the same data.

The concurrency row comes from `tests/parallel.sh`, which fails if concurrent
readers are not meaningfully faster than sequential ones.

Design decisions behind these, and the measurements taken while making them,
are in the design notes below.

## Tests

```bash
./tests/run.sh ./db          # 33 golden-file tests
./tests/recovery.sh ./db     # 20 crash-recovery tests (kills the process mid-session)
./tests/wire.sh ./db         # 49 protocol and concurrency tests (needs python3)
./tests/parallel.sh ./db     # checks that readers actually overlap
./tests/bench.sh ./db        # the table above
```

Crash recovery and the wire protocol can't be checked with golden files — one
needs the process killed, the other is a conversation over a socket — so they
have their own scripts. `parallel.sh` is a timing measurement rather than an
assertion: it fails if concurrent readers are no faster than sequential ones.

CI additionally builds under AddressSanitizer and UndefinedBehaviourSanitizer,
and runs the concurrency tests under ThreadSanitizer — which is the only
place data races are actually caught, since the Windows toolchain this was
written on has no working sanitizer of any kind.

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
