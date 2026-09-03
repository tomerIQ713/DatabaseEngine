# DatabaseEngine

A SQL database engine written from scratch in C99 — no dependencies, ~14,700
lines. Buffer pool, write-ahead logging with crash recovery, persistent B+ tree
indexes, checksummed pages, concurrent readers, and enough of the PostgreSQL
wire protocol that `psql` and standard drivers connect to it directly.

```console
$ ./db shop.db
db> select city, count(*), avg(age) from users group by city order by count(*) desc;
city | count(*) | avg(age)
haifa | 2 | 35.5
tel aviv | 2 | 27.5
eilat | 1 | 25
(3 rows)
```

## Quick start

```bash
cd DatabaseEngine
cc -std=c17 -O2 -o db *.c -lpthread      # -lws2_32 on Windows

./db :memory: < demo.sql                 # guided tour, error cases included
./db shop.db                             # persistent session
./db shop.db --port 5433                 # serve the PostgreSQL protocol
```

```bash
psql -h 127.0.0.1 -p 5433
```

Builds on Linux, macOS and Windows. A Visual Studio solution
(`DatabaseEngine.slnx`) is included.

## Features

| | |
|---|---|
| **SQL** | `SELECT`/`INSERT`/`UPDATE`/`DELETE`, `INNER` and `LEFT OUTER JOIN`, subqueries (`IN`, `EXISTS`, scalar), `GROUP BY`, `HAVING`, `ORDER BY`, `DISTINCT`, `LIKE`, arithmetic, `ALTER TABLE`, `VACUUM` |
| **Types** | `int`, `text`, `float`, `date`, `varchar(n)` |
| **Constraints** | `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `DEFAULT`, `CHECK` |
| **Storage** | 8 KB slotted pages, CLOCK buffer pool, per-page checksums |
| **Durability** | Write-ahead log, fsync per statement, `BEGIN`/`COMMIT`/`ROLLBACK` |
| **Indexes** | B+ trees stored as pages — saved and reopened, not rebuilt |
| **Planning** | Index selection, hash join on equality, top-N for `ORDER BY … LIMIT` |
| **Concurrency** | Thread per connection; readers run in parallel under a reader-writer lock |
| **Protocol** | PostgreSQL v3, simple and extended query |

`NULL` follows the standard's three-valued logic throughout: `x NOT IN (…NULL…)`
returns nothing, `count(col)` over an unmatched outer join row counts zero, and
`ON` and `WHERE` on an outer join mean different things.

## Architecture

Structured as a compiler with an execution stage on the end. Each connection
runs a statement through every stage; the engine lock decides which may run
together.

```
   lexer  ──▶  parser  ──▶  semantic  ──▶  executor  ──▶  result set
    03          04            05             06              08
                                              │
                        ┌─────────────────────┼─────────────────────┐
                        ▼                     ▼                     ▼
                     catalog               storage              B+ trees
                       02              (slotted pages)             11
                                             07
                                              │
                                     buffer pool (14)
                                    CLOCK + checksums
                                     │              │
                              page file (12)     WAL (15)
```

`16_wire.c` is a second front door onto the same pipeline: a statement arriving
over a socket takes the identical path as a typed one.

## Benchmarks

Reproduce with `tests/bench.sh` and `tests/parallel.sh`. Wall clock over whole
sessions on a laptop; absolute numbers vary by roughly a third between runs, so
the ratios are the point.

| | before | after |
|---|---|---|
| 500 point queries over 200,000 rows | 2.60 s (scan) | **0.95 s** (B+ tree, build included) |
| 2,000 inserts | 4.86 s (fsync each) | **0.12 s** (one transaction) |
| 4 concurrent readers | 1.89 s (sequential) | **0.65 s** (parallel) |
| join, 20,000 × 5,000 rows | 2.96 s (nested loop) | **0.22 s** (hash join) |

## Tests

```bash
./tests/run.sh ./db          # 33 golden-file tests
./tests/recovery.sh ./db     # 20 crash-recovery tests (kills the process mid-session)
./tests/wire.sh ./db         # 49 protocol and concurrency tests
./tests/parallel.sh ./db     # fails if concurrent readers do not overlap
python3 tests/fuzz.py sql  --exe ./db --runs 300
python3 tests/fuzz.py wire --exe ./db --runs 300
```

Crash recovery and the wire protocol cannot be checked with golden files — one
needs the process killed, the other is a conversation over a socket — so each
has its own harness. The fuzzer shrinks any crash to the fewest statements that
still reproduce it.

CI runs every suite on Linux, macOS and Windows, plus AddressSanitizer,
UndefinedBehaviourSanitizer and ThreadSanitizer builds.

## Limitations

- **Writes are serialised.** One reader-writer lock covers the engine, so a
  write excludes everything and a transaction holds the lock until it commits.
  Read-heavy loads scale; write-heavy ones do not.
- **16 connections.** The seventeenth is refused rather than queued.
- **No authentication.** The server binds to loopback only.
- No correlated subqueries, `RIGHT`/`FULL JOIN`, `UNION`, or subqueries in
  `FROM`. `IN` with a literal list is capped at ~16 values.
- psql's backslash commands (`\d`) query `pg_catalog` and do not work. Typed SQL
  does.

## Documentation

[`CLAUDE.md`](DatabaseEngine/CLAUDE.md) documents every invariant, why it
exists, and what broke when it didn't hold.

## License

MIT — see [LICENSE](LICENSE).
