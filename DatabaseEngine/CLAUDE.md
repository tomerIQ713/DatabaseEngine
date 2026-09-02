# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A SQL database engine in C99, structured as a compiler with an execution stage on
the end. The numbered source files are the pipeline stages, deliberately mirroring
the author's Python-to-C compiler: lexer, parser, semantic check, then an executor
where that project had a translator.

## Build and test

Visual Studio (`DatabaseEngine.slnx` one level up) produces `x64/Debug/DatabaseEngine.exe`.

For a fast check without the IDE:

```bash
gcc -std=c17 -Wall -Wextra -o db.exe *.c -lws2_32
```

`-lws2_32` is for `16_wire.c`, which is the only thing here that opens a
socket. The Visual Studio project carries the same library on all four
configurations.

Crash recovery cannot be checked with golden files, so it has its own script -
it kills the engine mid-session and reopens the database:

```bash
./tests/recovery.sh ./db.exe
```

The wire protocol cannot either - it is a conversation over a socket - so it
has its own script, which starts the engine as a server and talks to it:

```bash
./tests/wire.sh ./db.exe
```

That one needs `python3` for the client half, and skips itself without it.

The README quotes numbers, and `tests/bench.sh` is what reproduces them - it
generates its own data, so it needs nothing but the engine:

```bash
./tests/bench.sh ./db.exe
```

It times whole sessions rather than isolated operations, which is honest about
what is being measured and adequate for differences of the size it reports. If
you change something it covers, re-run it and update the README table: a number
nobody re-measures is worse than no number.

Neither suite generates its own input, so there is a third script that does
nothing else:

```bash
python3 tests/fuzz.py sql  --exe ./db.exe --runs 300
```

```bash
python3 tests/fuzz.py wire --exe ./db.exe --runs 300
```

It mutates SQL and protocol messages, runs them, and shrinks anything that
crashes to the fewest statements that still do it. A run is reproducible from
its `--seed`. **MinGW here has no working `-fsanitize=address` and no usable
`-fstack-protector`** - both link but fault on entry - so a fuzz build is
`-O2 -D_FORTIFY_SOURCE=2` and only hard crashes are caught. Under MSVC,
`/fsanitize=address` works and is worth a run.

Run the golden-file suite (defaults to the MSVC output path):

```bash
./tests/run.sh
```

```bash
./tests/run.sh ./db.exe
```

One test at a time — the runner has no filter, so drive the engine directly:

```bash
./db.exe :memory: < tests/07_index.sql
```

After an intentional behaviour change, read the diff first, then regenerate
baselines with `./tests/run.sh --bless [exe]`. Baselines capture the banner and
prompt too, so an edit to `showBanner` or the `db> ` prompt in `main.c` invalidates
all 30 of them.

## Running the engine

A bare run opens `database.db` and rewrites it on clean exit (`.exit`, `.quit`,
Ctrl+Z). Killing the process — including the Visual Studio stop button — skips the
save. Pass `:memory:` for a throwaway session, or any other path to use that file.

Dot-commands: `.tables`, `.indexes`, `.databases`, `.pool`, `.explain on|off`,
`.save <path>`, `.load <path>`, `.exit`.

`--port <n>` serves the PostgreSQL wire protocol on that port instead of
reading lines, and the official client connects to it:

```bash
./db.exe shop.db --port 5433
```

```bash
psql -h 127.0.0.1 -p 5433
```

`.explain on` prints the chosen access path for each query and is the fastest way to
confirm an index is actually being used. `.pool` reports the buffer pool — resident
and dirty frames, hits, misses, evictions — and is the only way to see whether pages
are actually being faulted in rather than sitting in memory.

## Architecture

`sql_common.h` is the single shared header: every limit, every error code, every
struct, and every module's declarations. There are no per-module headers.

Pipeline, one statement at a time through `ProcessStatement` in `10_controller.c`:

| Stage | File | Produces |
|---|---|---|
| lexer | `03_lexer.c` | `TokenList` |
| parser | `04_parser.c` | `Statement` (recursive descent) |
| semantic | `05_semantic.c` | validation against the catalog |
| executor | `06_executor.c` | `ResultSet` |

Supporting layers: `02_catalog.c` (table definitions, hash table), `07_storage.c`
(row heaps plus the shared value comparators), `14_pool.c` (buffer pool),
`11_index.c` (B+ trees),
`12_persist.c` (file format), `13_database.c` (database namespaces),
`08_result_set.c` / `09_view.c` (output), `01_errors.c` (code-to-string),
`16_wire.c` (the PostgreSQL protocol), `17_expr.c` (arithmetic).

### Invariants that are easy to break

**Error codes are the calling convention.** Nearly every function returns an `int`
error code; `SUCCESS_CODE` is 0. Codes are banded by stage in `sql_common.h` — 100s
lexical, 200s syntax, 300s semantic, 400s execution, 600s I/O. A new code needs an
entry there and a case in `errorCodeToString`.

**Rows are not an array — they are slotted pages, and the pages belong to the
buffer pool.** Each 8 KB page has a slot directory growing up from the front and
variable-length records growing down from the back (`07_storage.c`). A row of two
ints costs about a dozen bytes rather than the 2,244 a fixed `Row` occupies. A
`Heap` holds page *ids*, not pages: `14_pool.c` owns the memory.

**A pointer into a page is valid only while that page is pinned.** Every function
in `07_storage.c` pins, works, and unpins before returning; eviction can reuse a
frame the moment it is unpinned. Never hold a `Page*` across a call that might pin
something else.

Reach rows through `heapRead(heap, position, &row)`, never by indexing. A position
is `(page << 10) | slot`, so **positions are sparse** - page 0 might use 0..289 and
page 1 starts at 1024. Iterate with `heapFirst`/`heapNext`; a `for (r = 0; r <
heapSlots(heap); r++)` loop is wrong, and `heapSlots` returns the slot *count* for
sizing and reporting, not a position range.

**Reading rows: in place for a pass that discards them, copied out for one that
keeps them.** `heapRead` copies a row out and interns its text, which a scan
testing a WHERE does not need - it keeps only positions. So the executor walks
with a `HeapScan` instead:

- `heapScanStart` / `heapScanNext` walks live rows in order, keeping the page it
  is on pinned, so it costs one pin per *page* rather than per row, and skips
  tombstones for you.
- `heapScanAt(scan, position)` reads one row by position, holding the last page
  pinned - which is free for a list of positions, because they come out in page
  order.
- `decodeRecord(record, &row, upto)` fills a `Row` whose text points into the
  page. Nothing is allocated and nothing is interned.

Three rules come with that:

- **The values die at the next call.** Anything kept has to be copied - `setText`
  into the statement arena for a result row, `retain` into the group arena for a
  group key. `projectRows` interns only the columns being projected, which is
  why selecting one column of a wide row no longer copies the rest.
- **`upto` is the deepest column you will look at.** Fields are variable-length
  and have to be walked in order, so decoding stops there; values past it are
  whatever the previous row left behind. Work it out from the slots you resolved
  (`deepestSlot` does it for a condition).
- **Every `heapScanStart` needs a `heapScanEnd` on every path out**, including
  error returns. A leaked pin holds a frame for the rest of the session.

Measured against copying each row out: a scan with a WHERE is 2.3-3.7x faster,
and one with no WHERE - which now decodes nothing at all - is 14x.

`DELETE` clears a slot's offset and keeps the slot, so positions stay valid for the
indexes. `VACUUM` calls `heapCompact`, which renumbers everything — hence the index
rebuild straight after.

**The database file is the page file, and it is written in place.** Layout is a
header page then the data pages at `PAGE_SIZE * (1 + id)` — and nothing after
them. The catalog lives in a chain of ordinary pages (`12_persist.c`, version 5),
which is what makes in-place writes possible at all: with metadata at the end,
extending the page area would overwrite it. The chain also has no size cap, and
needs none, because a table's page list grows with the table.

Opening reads only the catalog, so a large database opens without touching a
page; pages fault in as queries reach them.

**Durability comes from the log** (`15_wal.c`). Every statement that changes
anything ends in `commitDatabase`: the catalog is written into its pages, every
unlogged dirty page becomes a log frame, and the log is fsynced. Only then does
the statement report success. A crash after that point is repaired by replaying
the log on the next open; a crash before it loses the statement whole. Nothing
partial survives — frames past the last commit marker are a torn transaction and
are skipped, and a frame whose checksum fails ends the replay.

Frames carry **whole pages**, not diffs. That costs log volume and buys
idempotent replay, so recovery needs no undo pass.

**Every page carries a checksum over itself, and the last eight bytes of a page
are the pool's, not the page's.** `PAGE_TRAILER` is a checksum plus four spare
bytes, and every layout that lives in a page stops short of it: the heap's
records grow down from `PAGE_USABLE` rather than `PAGE_SIZE`, `nodeOrder`
divides what is left after it, and `CATALOG_PAYLOAD` subtracts it. A page that
comes back from the file not matching what was written is refused by `poolPin`,
which every caller already treats as a page it cannot have.

Four things this rests on:

- **Stamped where it is written, not where it is changed.** `poolStampPage` is
  called in `flushFrame`, in `poolWriteAll`, and in `poolCommit` before the page
  goes into the log - a log frame carries a whole page and is replayed into the
  file, so it has to satisfy the same check. Stamping on every dirty unpin
  instead would run 8 KB of arithmetic per inserted row.
- **A zero checksum means unstamped**, and is not checked: a hole in the file
  has one, and so does every page of an older database.
- **Verification is switched on before the first page is read**, which means
  before the catalog chain - the catalog lives in ordinary pages, and a corrupt
  catalog page is the one most worth catching. It is switched on for a file
  this build wrote (`openForWrite`, `saveDatabase`) and off for an older one.
- **An older file is compacted on the way in.** Its pages put records exactly
  where the checksum now goes, so writing one back would destroy the row living
  there. `reservePageTrailers` rewrites every heap through `heapCompact` and
  rebuilds the indexes - the same call VACUUM makes - once, when the file is
  opened.

A refused page would otherwise look like an empty table, so `ProcessStatement`
compares `poolCorruptCount()` across the statement and turns a page it never
got into `ERROR_IO_CHECKSUM` rather than a short answer.

**This is what lifted the memory ceiling.** A dirty page marked `logged` may be
evicted: it is written into the database file on the way out, and the log can
repair a torn write. A dirty page that is *not* logged still cannot move — that
is the write-ahead rule, and `acquireFrame` enforces it. Measured: a 151-page
database runs in 64 frames with 64 dirty, where it previously had to hold all 151.

Things to keep true:

- **Order at checkpoint**: pages out, database file *fsynced*, *then* the log
  removed. Dropping the log first would leave nothing to recover from, and a
  plain `fflush` is not enough — it reaches the operating system, not the disk,
  so the log would be gone while the pages it holds were still in a cache.
- **A checkpoint that fails keeps its log.** At that point the log is the only
  complete record of what was committed, so `closeDatabase` removes it only on
  success.
- **A page is marked `logged` only after `walSync` returns**, never as each
  frame is appended. `logged` is what makes a page evictable, so marking one
  early lets a half-written transaction reach the database file — the write-ahead
  rule broken exactly when it matters.
- **The catalog reaches its pages before the commit**, not after. A committed
  INSERT whose new page the catalog does not mention comes back as a page
  belonging to nothing.
- **Only a real page file may be opened for writing.** `isPageFile` in
  `12_persist.c` answers that — `main.c` asks before attaching a log, and
  `walRecover` asks before replaying into one. A version 1 or 2 file has a
  different layout, and writing a page into it at a page offset destroys it — it
  did, once. Older files are rewritten whole at exit instead, which upgrades them.

The cost is one fsync per statement: about 2.5 ms against 0.05 ms in memory. That
is the price of per-statement durability, and the reason `BEGIN`/`COMMIT` exists.
`BEGIN` is what a bulk load uses to stop paying it on every row.

**A transaction is a suspended commit, and the file is its before-image.**
`BEGIN` checkpoints first - every page committed so far goes into the database
file and the log is emptied - and only then stops `ProcessStatement` calling
`commitDatabase`. That one ordering is what makes the rest simple: after the
checkpoint a dirty page can only be one this transaction wrote, so `ROLLBACK`
is `poolRollback` (drop every dirty page, take the page count back) followed by
`loadDatabase` (read the catalog, heaps and index roots again). There is no undo
log and no before-image, because the file already is one.

Three things that follow, all pinned by `tests/recovery.sh`:

- **A crash inside a transaction loses it whole**, because nothing since BEGIN
  reached the log.
- **COMMIT is the fsync**, so a committed transaction survives the same crash.
- **The reload is not laziness.** Heaps, index roots and the catalog live in
  memory as well as in pages, and re-reading them is the only thing that puts
  all three back at once.

`:memory:` has no file to go back to, so ROLLBACK there is an error and the
transaction stays open. Uncommitted work is rolled back at exit rather than
being folded into the file by the closing checkpoint.

Replacement is CLOCK, and `pageFrame` maps page id directly to frame, so a pin is
one load rather than a scan of every frame — worth about 23% on a scan-heavy
workload once the pool has a few hundred frames. Note what this does *not* fix: a
cyclic scan over more pages than the pool has frames gets no reuse under CLOCK or
LRU, and that is inherent to the access pattern rather than a flaw in the policy.
The standard remedy is a scan ring, which protects a *non-scan* working set. Now
that index nodes are pages too, that set exists — a big table scan can flush the
index pages a point lookup wants — so a ring has become worth adding.

**The pool, the log, and the open file are one set, and `.save` and `.load`
move them.** Four data-loss bugs came out of that pairing coming apart, and
`tests/recovery.sh` now holds one case each:

- Saving over the file you are paging from has to `poolDetachFile` first,
  because Windows will not rename over an open file — and then hand the pool
  back a handle opened the *same* way. Reopening a writable pool read-only left
  it believing it could still write, so every later statement failed silently.
- `poolMarkAllClean` is only right when the save replaced the file the pool is
  reading from. After `.save elsewhere.db` those pages are in a file this
  session is not using; marking them clean lets them be evicted and dropped.
- `poolAdopt` takes a read-only handle, so it clears `writable`. `loadDatabase`
  folds the current log away before switching, or the log would go on describing
  the database it had just closed.
- A save writes the whole file, so any log for that path is describing a
  database that no longer exists — `walDiscard` drops it. Left behind, it is
  replayed into the next file to carry that name.

And the pool keeps its *own* file handle: sharing the one `loadDatabase` reads
with was a real bug, because faulting a page seeks while the metadata is still
being read sequentially.

Versions 1 through 4 still load and are written back as version 5.

**Nothing truncates SQL. Ever.** A statement that does not fit is refused -
`ERROR_TOO_MANY_TOKENS` from the wire's `nextStatement`, `ERROR_TOKEN_TOO_LONG`
from the lexer's three copy loops. This is not tidiness: a truncated statement
usually still parses, and `select ... where a and b` with the `b` cut off is a
wrong answer rather than an error. The same query then answers differently
depending on how much whitespace it was written with, which is the worst thing
a database can do with a query. Both loops used to stop copying and carry on
reading; `tests/wire.py` pins the refusal.

**A row reaches the heap and its indexes together, or not at all.** `heapInsert`
then `indexInsertRow` leaves a window where the row is in the heap and missing
from a tree - visible to a scan, invisible to a lookup, with no error afterwards
to say so. Both `executeInsert` and `executeUpdate` close it by tombstoning the
row they just stored when indexing fails, and `executeCreateIndex` drops an
index it could not finish filling. Entries other indexes did take are left
behind on purpose: a stale entry is safe, because every candidate is re-checked
against the heap.

**A `ResultSet` carries its column types, not just its headers.** A query that
matched nothing still has a shape, and the wire protocol is asked what a
statement returns before there is a row to look at - reading the type off
`rows[0]` answered "text" for everything in that case, and a driver decoded
every integer as a string. `projectRows` and `projectGroups` fill `types[]`
alongside `headers[]`.

**A value's text is `(text, textLength)`, and it is not NUL-terminated.** A row
read by a scan points straight at the bytes in its page, and a record stores text
with a length in front of it and no terminator, so `strcmp`, `strlen` and `%s`
all read past the end. `compareValues`, `valuesSame`, `hashValue`, `likeMatch`
and the printer are all length-bounded; anything new must be too.

That is also where a comparison trick lives: `valuesSame` settles `=` and `<>` on
the lengths, and only equal-length text reaches a `memcmp`. `compareValues`
orders by shared prefix and then by length, which is the same order `compareKeys`
gives in `11_index.c` - the scan and the index have to agree, or an index would
answer a range query with a different set of rows than the scan it replaces.

**Text lives in an arena, not in the value.** Anything a statement parses is
interned in the statement arena (`internText`), which `ProcessStatement` empties
before each statement - so a parsed text value is valid for exactly as long as
the statement that produced it.

Two things outlive that, and both take their own copy:

- **Index keys.** `indexInsert` copies the key into its node's page, so a key
  outlives the statement by being part of the tree rather than by being held
  anywhere. Splits move whole fixed-size entries between pages.
- **Defaults and CHECK literals.** The catalog has an arena of its own
  (`02_catalog.c`); `addTable` copies the text of every DEFAULT and every
  literal inside a CHECK into it, because a table outlives the CREATE that made
  it. Released with the catalog.
- **Group keys and running min/max.** `retain()` in the executor copies into
  `groupArena`, because the scan winds the statement arena back after every row.

That winding back is the other half of the design. A scan that reads a row, tests
it, and keeps only its position brackets the read with `textMark`/`textReset` —
otherwise every rejected row's text would pile up for the whole statement. The
places that do this are `collectRows`, `projectGroups`, `joinTables`,
`executeUpdate`, and both index builds. **If you add a loop that reads rows and
discards them, bracket it the same way; if you add one that keeps values, do not.**

**What is still fixed-size:** `MAX_COLS`, `MAX_TABLES`, `MAX_DATABASES`, and
`MAX_CONDITION_NODES`. `Condition` keeps its nodes in an inline pool so it copies
by value with no allocation. Text is bounded only by what fits on one input line
(`LINE_LEN`) and in one page.

**Groups grow and are hashed.** `groupKeys` and `accumulators` are flat arrays
indexed `group * MAX_COLS + column`, reached through `keyAt`/`accumulatorAt`, and
`groupBuckets` is an open-addressed index into them. The two go together: lifting
the old fixed limit without the hash would have turned a clean "too many groups"
error into a quadratic scan that looks like a hang.

**Result sets and scan buffers grow.** `resultReserve` doubles `ResultSet.rows`;
`reserveCandidates` does the same for the executor's shared array of candidate row
positions. Anything writing to `out->rows` must reserve first — `projectGroups`
needs it as much as `projectRows` does. `freeResultSet` and `freeExecutor` release
them.

**Databases are namespaces, not stores.** The catalog, heap list, and index list
each carry a `[MAX_DATABASES]` dimension indexed by `currentDatabaseId()`. Slot ids
are array indices, so a dropped slot is marked free and never renumbered —
renumbering would silently repoint tables at another database's data.

**A join is flattened into a fake single table.** `FROM a, b` and
`a [INNER] JOIN b ON ...` parse to the same thing: a table list plus a condition.
`addCondition` in `04_parser.c` ANDs each ON clause into the same node pool WHERE
uses, so ON is filtering by the time the executor sees it and there is no separate
join-condition concept anywhere below the parser. This builds a synthetic
`CatalogNode` whose columns are named `a.id`, `b.name`, and a synthetic `Heap` of
combined rows (`buildJoinSchema` in `02_catalog.c`, `buildJoin`/`joinTables` in
`06_executor.c`). Everything after that — filtering, grouping, ordering,
projection — is the single-table code path, unmodified. The synthetic table is
named `<join>`, which no identifier can spell, so `findIndexOn` never matches it
and a join always scans. `select->ntables == 1` keeps ordinary queries off this
path entirely.

WHERE is evaluated as each combined row is completed, so a join is bounded by the
rows it keeps rather than the pairs it considers.

**Two tables joined on an equality do not consider the pairs at all.** If the AND
spine of the WHERE holds an `=` whose two sides sit in different tables,
`buildJoin` takes the hash path: the second table goes into a hash table keyed on
its side of the equality, the first is scanned once, and each of its rows looks up
only the rows that could match. That is O(n + m) against the nested loop's
O(n * m) - a 20,000 x 5,000 join went from 2.96 s to 0.65 ms.

Four things keep it honest:

- **Only the AND spine is walked**, for the same reason index selection only
  walks it: under an OR the other branch can match anything.
- **Every pair it forms still goes through the whole WHERE**, the equality
  included. The hash decides what to look at, never what the answer is.
- **NULL keys are dropped on both sides**, because `=` against NULL is unknown
  and an unknown row is not returned - which is what the nested loop concludes
  too, one pair at a time.
- **The second table is the build side**, not the smaller one. That keeps the
  output in the order the nested loop produced whenever the build key is unique,
  which is what the golden files record.

Anything else - three tables, an OR, an inequality, no WHERE - is still the
nested loop, which now also reads its rows in place.

**Column references may or may not be qualified.** The lexer treats a dot between
identifier characters as part of the word, so `users.id` arrives as a single
token and nothing downstream parses qualification. `findColumn` resolves the rest:
exact match first, then a bare name against a join schema (ambiguity returns
`COLUMN_AMBIGUOUS`, never a guess), then a qualified name against a single table.
`findHeader` in the executor applies the same rule to ORDER BY and HAVING, which
match result headers rather than catalog columns.

**Keyword and command lookup dispatch on a packed integer.** `classifyWord` in
`03_lexer.c` stages a word's first four bytes through a zero-filled buffer, loads
them into a `uint32_t` with a fixed-size `memcpy`, folds case with one
`|= 0x20202020`, and switches on the result; `runDotCommand` in `main.c` does the
same for `.tables` and friends, without the fold. `PACK_4` lives in
`sql_common.h` and builds the matching constant at compile time.

Three things to keep true when adding a keyword or command:

- **The four-byte prefixes must stay distinct.** All 47 keywords currently are,
  so a case identifies exactly one. If a new one collides, that case has to test
  both candidates.
- **Every case must still validate the tail.** `selection` packs identically to
  `select`; only the length check and `_strnicmp` tell them apart. Test
  `24_keyword_dispatch` is built out of near-misses for exactly this reason.
- **Short words are staged, not read directly.** `by` is two bytes, so reading
  four would run past it. The NUL padding folds to `0x20`, which `PACK_4_LOWER`
  reproduces in the label.

This is not a jump table, despite what the shape suggests: 47 values spread
across the 32-bit range are far too sparse for one, and gcc emits a balanced tree
of integer compares instead — about six, against a `_stricmp` for each before.
Measured on the lexer alone it is 2.1x (1400 to 660 ns/statement); end to end it
is invisible, because lexing is not what this engine spends its time on.

**Aliases requalify columns.** `from users u` and `select x as y` both parse; an
alias defaults to the table's own name. `joinSchemaNeeded` decides the path: a
single unaliased table resolves straight against the catalog (and prints
unqualified headers), while a join *or* one aliased table goes through the
flattened schema, because that is the only place an alias exists. Column aliases
set `SelectItem.aliased`, and only then does the label beat the column's own name
as the header — so `select users.name from users` still says `name`.

**A predicate's right side may be a column.** `Predicate.rightColumn` is empty for
the usual literal comparison and set for `a.id = b.uid`. `ConditionSlots` carries
a resolved position for each side; `right` is -1 when the literal applies. Two
consequences worth remembering: `findIndexableLeaf` refuses such a predicate
because there is no constant to seek to, and `comparePredicate` returns UNKNOWN
when *either* side is NULL, which is what stops rows pairing on missing values.

**B-tree nodes are pages too.** An index stores only its root page id; the nodes
live in the pool like heap pages, so a tree is saved and reopened rather than
rebuilt by scanning the table (`11_index.c`, file version 4). Reopening a
200,000-row table with an index went from 0.215s to 0.090s — faster than the
same table with *no* index, because the point query no longer scans.

Three things hold this together:

- **Entry size follows the key type.** One index is one column, so every key in
  a tree has the same type: an int entry is 8 bytes, a float 12, a text 52. The
  node's own header says which (`NODE_TEXT`, `NODE_FLOAT`), so a split makes a
  sibling of the same shape without being told. Getting this wrong cost 27 MB
  for a 200,000-row int index; it is now 3.2 MB.
- **A date is an int key.** It is a day count, so `makeKey` writes it through
  the int path and the tree never learns that dates exist.
- **A float key is stored as an order-preserving integer.** IEEE doubles are
  sign-and-magnitude, so flipping every bit of a negative and setting the top
  bit of a positive makes unsigned byte order agree with numeric order
  (`floatOrder`). That is what lets one comparison serve every key type.
- **Long text keys are truncated** to `INDEX_KEY_MAX` bytes. Safe *only* because
  of the candidate rule below: prefix order is monotone with full order, so
  truncation adds candidates and never hides one — provided the scans stop on a
  strict inequality and admit ties. `indexScan` is deliberately generous for
  exactly this reason; do not "tighten" its comparisons.
- **`lowerBound` binary-searches.** Fanout is up to 1022, so the linear walk it
  replaced was ~500 comparisons a node and made a bulk load 4x slower.

Two consequences of persisting trees. A tombstoned row keeps its index entry
across a save now, where the old rebuild-on-load quietly dropped it — `.indexes`
therefore reports more keys than rows until a `VACUUM`. And `keyCount` is kept
on the `Index` rather than counted by walking, which is what stopped `.indexes`
faulting 2,600 pages on a large tree.

**Index entries are candidates, not answers.** `collectRows` narrows with an index
when it can, then re-runs the full condition over every candidate row. This is what
makes stale entries safe: DELETE and UPDATE tombstone rows without touching the
trees, and UPDATE additionally appends the new row and its index entry. Only VACUUM
compacts the heap and rebuilds indexes. Do not "optimise" away the re-verification.

**Index selection only walks AND spines.** `findIndexableLeaf` never descends into
OR or NOT, because a matching leaf there does not constrain the result.

**LIMIT stops the scan, but only when nothing above it reorders.** ORDER BY,
DISTINCT, GROUP BY and aggregates all decide *which* rows come back, so they need
every matching row; a plain `limit 10` does not, and `collectRows` stops after
ten. `executeSelect` works this out and passes `stopAt`.

**ORDER BY sorts a small array beside the rows, not the rows.** A `Row` is over
half a kilobyte and the comparator looks at one value in it, so `applyOrder`
builds a `SortKey` per row - the first ORDER BY value inline, plus the row's
index - sorts those, and then moves each row once by following the permutation's
cycles. Later terms and ties read the row itself, which is rare enough to afford
the reach.

Two consequences. **The sort is now stable**: ties come out in the order the rows
arrived, so the same query twice prints the same thing, where `qsort` on rows left
it to chance. And **ORDER BY with a LIMIT is a top-N**, not a sort: the best k are
kept in a heap whose root is the worst of them, which costs one comparison for a
row that cannot get in. Ordering 50,000 rows to print five went from 745,000
comparisons to about 50,000. `tests/26_join_and_limit.sql` pins both, and the
top-N must always agree with the same query without the LIMIT.

**DISTINCT and grouping hash; they do not compare pairwise.** Both go through
`hashValue` and `valuesEqual`, so there is one notion of sameness rather than
three. The group table's row is `ngroup` keys and `nitems` accumulators wide, not
`MAX_COLS` of each - through `keyAt`/`accumulatorAt`, whose stride is set per
query - and each group's hash is kept so that growing the table does not have to
work it out again.

**count(\*) with no GROUP BY never reads a row.** It names no column, so
`projectGroups` takes the count from the scan and stops.

**An accumulator keeps two totals.** `sum` answers in the column's own type and
`avg` divides, so `Accumulator` carries both a `long long` and a `double`: a sum
over an int column stays exact, and an average is a real number whatever it was
averaging. `avg` over no rows is NULL rather than zero - there is nothing to
divide by - and both skip NULLs, while `count(*)` counts them.

**Printing is hand-formatted into a 64 KB buffer.** `printf` was costing 5.2
microseconds a row, fifty times what reading the row cost, so a large result set
spent nearly all its time in the C runtime. `08_result_set.c` appends bytes whose
length it already knows and writes in 64 KB blocks - 43x on a 50,000-row result.
Two exceptions. A statement that only reports what it did is one short line, and
one `printf` beats setting up a buffered write - measured at 0.9 microseconds a
statement, which is most of what an INSERT costs. And a float goes through
`snprintf`: getting a double to read back the way it was written is not
something to hand-roll for the sake of a format string.

**Three comparison semantics coexist on purpose.** WHERE uses tri-state logic where
NULL matches nothing; `valuesEqual` (GROUP BY, DISTINCT) treats NULLs as equal;
`orderCompare` (ORDER BY) sorts NULL first. Reaching for the wrong one silently
produces wrong rows rather than an error.

### Column types

`int`, `text`, `float`, `date` and `varchar(n)`, and only the first two are
distinct at the bottom of the engine:

- **`varchar(n)` is text with a ceiling.** `Column.size` holds the limit, 0
  means unbounded, and nothing below the catalog sees a third string case. The
  limit is enforced where a value is *stored* (`checkConstraints`), not where
  one is compared - a comparison against a too-long string matches nothing,
  which is an answer rather than an error.
- **`date` is an int.** It is stored, compared, hashed and indexed as days
  since 1970-01-01 (`intLike` in `07_storage.c`), and differs from an int only
  when parsed or printed. `'YYYY-MM-DD'` is validated on the way in, so the
  31st of February is an error rather than a stored number. Anything branching
  on `TYPE_INT` almost certainly wants `intLike` - `hashValue` did not, once,
  and put every date in one bucket.
- **`float` is a double**, and is the only type that uses `Value.floatValue`.

**A literal is fitted to its column in the semantic stage, and nowhere else.**
`coerceLiteral` is what turns `'2024-05-01'` into a date and `3` into `3.0` for
a float column, so no execution path knows that a date was ever written as
text. That is why `semanticCheck` takes a mutable `Statement`. `LIKE` is
excluded on purpose: it is a pattern over stored text and says nothing about a
number or a day count.

### Constraints

`PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `DEFAULT <literal>` and `CHECK (...)`, in
any order after a column's type, plus a table-level `CHECK` as its own entry in
the column list.

- **PRIMARY KEY is UNIQUE and NOT NULL together.** The `COL_PRIMARY` flag only
  records which of the three the statement actually wrote, for `.tables`.
- **Every CHECK lands in one tree.** Column-level and table-level alike are
  ANDed into the table's single `Condition` by `addCondition` - the same node
  pool, the same evaluator a WHERE uses. A `CatalogNode` holds it by pointer,
  because a `Condition` is some kilobytes and most tables have none.
- **A CHECK rejects only what is definitely false.** A comparison against NULL
  is unknown, and an unknown check passes, so the test is `== TRI_FALSE` rather
  than `!= TRI_TRUE`.
- **UNIQUE creates an index.** `CREATE TABLE` makes `<table>_<column>_key` for
  every unique column, so a uniqueness test is a tree probe and a bulk load
  into a table with a primary key is not quadratic. The probe obeys the
  candidate rule like every other index use: entries can be stale and text keys
  are truncated, so each candidate row is read and compared.
- **NULLs never collide.** `=` against NULL is unknown, so any number of rows
  may hold NULL in a unique column.
- **UPDATE checks every row before it writes any.** A failure at row fifty with
  forty-nine already rewritten is not something this engine can undo outside a
  transaction, so `executeUpdate` makes a checking pass first. The one thing
  that pass cannot see is the rows colliding with *each other* - every row gets
  the same literal - so a unique column plus more than one row is refused
  outright.

**DEFAULT needs a column list to be reachable**, which is why INSERT has one.
Without a list the values are still positional and there must be one per
column: a miscounted INSERT stays an error rather than becoming a row of NULLs.

### Adding a statement

Touches five places, in order: keyword and `TokenType` in `sql_common.h` plus the
keyword table in `03_lexer.c`; a statement struct, a `StatementType`, and a union
member in `sql_common.h`; a `parseX` in `04_parser.c` plus dispatch in
`parseStatement`; a case in `semanticCheck`; an `executeX` plus dispatch in
`executeStatement`. Then a `tests/NN_name.sql` / `.expected` pair.

### Persistence format

`12_persist.c` writes field by field with explicit little-endian integers, never
struct dumps — padding and member order would otherwise become part of the file.
A version 5 file carries each index's root page and key count, so trees come back
with the pages; only a version 4-or-older file falls through to `rebuildIndexes`
after the rows are loaded.

`ORDER BY` and `HAVING` both parse their operand with `parseSelectItem` and match
on the label, which is why `order by count(*)` and `order by <alias>` work. `--`
runs to end of line in the lexer; a line that is only a comment yields no tokens,
and `ProcessStatement` treats that as a no-op.

Saving writes `<path>.tmp` and renames it over the target, so a kill mid-save
leaves the previous file intact rather than a truncated one. `MoveFileExA` is
declared by hand in `12_persist.c` rather than pulled in from `<windows.h>`,
because `winnt.h` defines a `TokenType` that collides with the lexer's.

Version 8 is current (`SAVE_VERSION` in `12_persist.c`); versions 1-7 still load
and are written back as 8. Changing the layout means bumping `SAVE_VERSION`,
updating both save and load, and deciding what happens to older files.

Four versions matter separately, so they have separate names. `CHAIN_VERSION`
(5) is where the catalog moved into the pages, and decides whether the catalog
is read from a page chain or from a tail; `META_VERSION` (6) decides whether a
column carries a size, flags and a default, and a table a CHECK;
`CHECKSUM_VERSION` (7) decides whether the pages carry checksums;
`SAVE_VERSION` (8) is the current one, where a CHECK holds expressions. Testing the wrong one makes a
version 5 file take the tail path and fail to open.

**An older file is never written a page at a time.** `attachLog` in `main.c`
asks `isPageFile`, which insists on the current version, and refuses the log
otherwise - so a version 5 file gets no in-place writes and is rewritten whole
at exit, which upgrades it. Without that check today's catalog would be written
into yesterday's file, whose header still claims the older version, and the next
open would read it as one and misparse every column.

### The PostgreSQL wire protocol

`16_wire.c` speaks enough of the version 3 frontend/backend protocol that the
official `psql` connects and gets rows back. It is a front door onto the same
session, not a second engine: a served statement goes through `ProcessStatement`
exactly as a typed one does, and `main.c` opens and saves the database around it
either way.

Every message is a type byte, an int32 length **that counts itself**, and a
body, all big-endian - the opposite of the file format, because this is someone
else's protocol. Values go in text format, so the bytes a row prints are the
bytes that go on the wire and there is no binary encoding to write.

Four things that are easy to get wrong, and all four are in `tests/wire.py`:

- **The SSL request must be answered.** psql sends `SSLRequest` before the
  startup packet unless told otherwise, and it is a bare int32 pair with no
  type byte. A server that ignores it leaves psql waiting for a byte that never
  comes, which looks like a hang rather than a refusal. The answer is one byte:
  `N`.
- **One `Query` may carry several statements**, and each wants its own result.
  `nextStatement` cuts on semicolons that are actually separators - a semicolon
  inside `'a;b'` is data - and drops comments while it goes, because the lexer
  stops at `--` and would throw away the rest of a multi-line query with it.
- **`ReadyForQuery` ends every exchange**, including a failed one, and its
  status byte is read from `inTransaction()`. That byte is how psql knows to
  change its prompt inside a transaction.
- **A dropped connection is not a commit.** A session that ends mid-transaction
  is rolled back before the next client is accepted, or the next one inherits
  it.

**The extended query protocol is there too**, which is what every driver that
is not psql uses: Parse, Bind, Describe, Execute, Sync, Close, Flush.

Parameters are substituted into the statement text at Bind rather than passed
down as values. This engine plans nothing and caches nothing, so a prepared
statement is only a string with holes in it, and filling them in is the whole of
what binding can mean here. Three things follow:

- **How a parameter is spelled depends on its type**, and the type is whatever
  Parse declared. A `$1` with OID 23 is written as a bare number; one with OID
  25 is quoted, with its own quotes doubled. When Parse declares nothing - which
  is common - the text is read to see whether it looks like a number, so an
  unquoted `'123'` stays text if the client said so and becomes a number if it
  said nothing. Getting it wrong is not silent: the statement is refused.
- **Binary parameters are refused by name.** Decoding them needs a reader per
  type OID, and every client can send text instead.
- **A portal runs at Describe or Execute, whichever comes first, and the whole
  result is kept.** Nothing streams, because the executor builds a `ResultSet`
  rather than leaving a scan open. `Execute` with a row limit still stops where
  it was asked to and answers `PortalSuspended`.
- **A simple `Query` destroys the unnamed portal**, which the protocol says and
  this engine needs: a result's text lives in the statement arena, and running
  anything else winds that arena back. Rows kept from an earlier `Execute`
  would be pointing at memory the next statement has already reused - which is
  what `tests/fuzz.py wire` found.

**An error skips the rest of the batch.** Everything after it is ignored until
`Sync`, which is what the protocol says and what clients rely on - they send
Parse, Bind, Execute and Sync in one write and then look at what came back.

Three limits worth knowing before demonstrating it. **psql's backslash commands
will not work**: `\d` and its relatives are SQL against `pg_catalog`, with
joins, casts and subqueries this engine does not have. Typed SQL does work.
**Describing a statement runs it, when it is a SELECT.** The shape of a result
is not known until it runs and this engine plans nothing it could ask instead -
but a SELECT changes nothing, so it is run with stand-in parameters purely to
see what columns come back, and the answer is thrown away. Anything that would
write is described as returning nothing. Drivers need this: pg8000 describes the
*statement* rather than the portal, and answering `NoData` to a SELECT leaves it
with nowhere to put the rows that arrive a moment later.

The stand-ins are not NULL, because `id >= NULL` is refused by the parser on
purpose. They are a literal chosen from the declared parameter type, and when
nothing was declared - pg8000 declares nothing - the candidates are tried in
turn until the statement parses. A probe therefore costs a scan, and a
parameterised query is run twice. And **one connection is
served at a time**, because the engine is a single set of globals - one catalog,
one pool, one open transaction - so two sessions at once would be two sessions
sharing one database's worth of state.

`winsock2.h` drags in `winnt.h`, which has an enumerator called `TokenType`, and
so does the lexer. The file renames the Windows one with a `#define` before the
include and undefines it after - the same collision that keeps `<windows.h>` out
of `12_persist.c`.

### Expressions

`17_expr.c` is arithmetic - `+ - * / %`, unary minus, brackets - over columns
and literals, wherever a value can be read: select items, both sides of a
comparison, inside an aggregate, and the right of a SET.

It sits between the semantic stage and the executor because both need it and
neither owns it. The parser builds the tree, `exprResolve` binds each column to
a position and stamps its type, `exprType` says what the tree produces, and
`exprEvaluate` turns a row into a value. Like a `Condition` it is a flat pool
with indices for edges, so it copies by value, allocates nothing, and is written
to disk as itself when a CHECK carries one.

**A predicate is two expression indices and an operator.** `a = 1`, `a = b` and
`a * 2 > b + 1` are one shape rather than three, which is what let
`ConditionSlots` go: evaluating a comparison is evaluating both sides and
comparing what comes out. The places that still care about the simple cases ask
what an operand is rather than reading a different field:

- **An index needs a plain column and a constant.** `findIndexableLeaf` checks
  `exprIsColumn` on the left and `exprIsLiteral` on the right, so `a * 2 = 10`
  scans - turning it into `a = 5` is algebra this engine does not do.
- **A hash join needs two plain columns.** `findEquiJoin` says so for the same
  reason: a computed key would have to be computed for every row, which is a
  different optimisation.
- **A header is the column's own name when the item is one**, and otherwise the
  expression spelled back out - `qty*price`, brackets included where a child is
  itself a sum or a product. That label is what ORDER BY and HAVING match
  against, so `order by qty * price` finds the item that produced it. Ordering
  by an expression that is *not* in the result is still an error: ORDER BY names
  output columns, as it always has.

Type rules are one sentence: an expression is a float if any part of it is, an
int otherwise, and text and dates are not arithmetic. A date is a day number, so
`day + 3` could mean three days - but that is a decision this engine has not
made, and guessing it is worse than refusing. Ints are computed wide and range
checked once (`applyInts`), because a sum of two ints does not fit in an int;
division by zero is an error rather than a value.

**NULL spreads.** Anything computed from an unknown is unknown, so a row with no
price has no `qty * price` either, and the comparison above it answers UNKNOWN
exactly as a bare NULL column would.

**Every assignment in an UPDATE sees the row as it was.** `applyAssignments`
computes all of them before storing any, so `set a = b, b = a` swaps rather than
copying one value into both.

**A HAVING term is parsed into a pool that is thrown away.** Only its label is
wanted, and building it in the condition's own pool left the item's columns
behind - which `resolveHavingExprs` then tried to find among the output headers,
where they are not. That is why `parseComparison` uses a scratch pool for the
aggregate case.

## Platform notes

Targets MSVC, MinGW gcc, Linux gcc and macOS clang. Three shims carry it, and
each is in the one place that needs it:

- **`_stricmp`/`_strnicmp`** are what every stage spells case-insensitive
  comparison as, so `sql_common.h` maps them onto `strcasecmp` off Windows.
- **Sockets** in `16_wire.c`: a `SOCKET` becomes an `int`, `closesocket`
  becomes `close`, and `WSAStartup`/`WSACleanup` become nothing. The winsock
  spelling stays in the body because it is the one that needs the explanation.
- **`fsync` and atomic rename** were already portable - `15_wal.c` picks
  between `_commit` and `fsync`, and `12_persist.c` between `MoveFileExA` and
  `rename`.

CI (`.github/workflows/ci.yml`) builds on all four and runs the suites, plus a
Linux job under `-fsanitize=address,undefined` - which is the only place the
engine runs under a working sanitizer, since this MinGW's is broken.

`_CRT_SECURE_NO_WARNINGS` is defined at the top of `sql_common.h` because the
project builds with `/sdl`, which promotes the C4996 deprecation warnings for
`sscanf` and friends into errors.

`long` is 32-bit on Windows, so `strtol` clamps a too-large literal to exactly
`INT_MAX` and slips past a naive range check. Integer parsing uses `strtoll` with
an `errno == ERANGE` test.

`ZERO`, `ONE`, and `TWO` are defined in `sql_common.h` and used in place of the
literals throughout; match that when editing existing files.
