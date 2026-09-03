#include "sql_common.h"
#include <limits.h>

typedef struct {
    int       count;
    long long sum;                      /* exact, for sum over an int column */
    double    real;                     /* the same total as a double, for avg */
    Value     extreme;                  /* running min or max */
    int       seen;                     /* 0 until the first non-NULL lands here */
} Accumulator;

/*
 * The group table grows, and is found through a hash rather than scanned. Both
 * matter together: lifting the old fixed limit without the hash would turn a
 * clean "too many groups" error into a quadratic scan that looks like a hang.
 *
 * Keys and accumulators are flat arrays indexed by group * MAX_COLS + column.
 * The bucket array holds group index + 1, so zero means empty.
 */
static THREAD_LOCAL Value*      groupKeys;
static THREAD_LOCAL Accumulator* accumulators;
static THREAD_LOCAL int         groupCapacity;

static THREAD_LOCAL int*        groupBuckets;
static THREAD_LOCAL int         bucketCount;

/*
 * How many keys and accumulators one group takes. These were MAX_COLS each,
 * which meant a query grouping on one column still allocated sixteen values a
 * group and copied all sixteen every time the table doubled: half a kilobyte a
 * group where thirty-two bytes would do.
 */
static THREAD_LOCAL int groupKeyStride;
static THREAD_LOCAL int groupAccStride;

/* Each group's hash, so growing the table does not have to compute it again.
   Rehashing 50,000 text keys ten times over is most of a millisecond. */
static THREAD_LOCAL unsigned int* groupHashes;

/*
 * Grouping keeps values from rows it has otherwise finished with - the key of
 * each group, and the running min or max. Their text cannot stay in the
 * statement arena, because the scan winds that back after every row, so it is
 * copied here and released when the next grouping starts.
 */
static THREAD_LOCAL Arena groupArena;

static Value* keyAt(int group, int column)
{
    return &groupKeys[(size_t)group * groupKeyStride + column];
}

static Accumulator* accumulatorAt(int group, int column)
{
    return &accumulators[(size_t)group * groupAccStride + column];
}

/* FNV-1a over whatever the value actually carries. NULLs all hash alike, which
   is what puts them in one group the way valuesEqual says they belong. */
static unsigned int hashValue(const Value* value)
{
    unsigned int hash = 2166136261u;

    if (value->isNull)
        return hash ^ 0x9E3779B9u;

    /* A date is a day number, and it has to hash as one: sent to the text
       branch below it would find no text, hash to a constant, and put every
       date in a table into one bucket. */
    if (value->type == TYPE_INT || value->type == TYPE_DATE) {
        unsigned int bits = (unsigned int)value->intValue;

        for (int i = ZERO; i < 4; i++) {
            hash ^= (bits >> (i * 8)) & 0xFFu;
            hash *= 16777619u;
        }
        return hash;
    }

    if (value->type == TYPE_FLOAT) {
        unsigned char bits[8];

        memcpy(bits, &value->floatValue, 8);

        for (int i = ZERO; i < 8; i++) {
            hash ^= bits[i];
            hash *= 16777619u;
        }
        return hash;
    }

    /* bounded by the length rather than a terminator: a value pointing into a
       page has no terminator to find */
    const char* at  = value->text;
    const char* end = at + value->textLength;

    for (; at != end; at++) {
        hash ^= (unsigned char)*at;
        hash *= 16777619u;
    }
    return hash;
}

static unsigned int hashKey(const Row* row, const int* groupSlots, int ngroup)
{
    unsigned int hash = 2166136261u;

    for (int k = ZERO; k < ngroup; k++)
        hash = (hash ^ hashValue(&row->values[groupSlots[k]])) * 16777619u;

    return hash;
}

static void freeGroups(void)
{
    free(groupKeys);
    free(accumulators);
    free(groupBuckets);
    free(groupHashes);

    groupKeys     = NULL;
    accumulators  = NULL;
    groupBuckets  = NULL;
    groupHashes   = NULL;
    groupCapacity = ZERO;
    bucketCount   = ZERO;
}

static void retain(Value* target, const Value* source)
{
    *target = *source;

    if (!source->isNull && source->type == TYPE_TEXT)
        target->text = arenaCopy(&groupArena, valueText(source), source->textLength);
}

static THREAD_LOCAL int explainEnabled;

static void freeOrder(void);

/*
 * Candidate row positions, shared by every scan. One growable buffer rather
 * than an array per caller, because a table scan can now return far more
 * positions than a stack frame will hold.
 */
static THREAD_LOCAL int* candidates;
static THREAD_LOCAL int  candidateCapacity;

/*
 * Row positions an index handed back for a uniqueness probe. Separate from the
 * shared candidate array on purpose: UPDATE is holding its rows in that one
 * while it asks whether each new value is already taken.
 */
static THREAD_LOCAL int* probeRows;
static THREAD_LOCAL int  probeCapacity;

static int reserveProbe(int rows)
{
    if (rows <= probeCapacity)
        return SUCCESS_CODE;

    int grown = probeCapacity ? probeCapacity : INITIAL_RESULT_ROWS;
    while (grown < rows)
        grown *= TWO;

    int* moved = (int*)realloc(probeRows, (size_t)grown * sizeof(int));
    if (moved == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    probeRows     = moved;
    probeCapacity = grown;
    return SUCCESS_CODE;
}

static int reserveCandidates(int rows)
{
    if (rows <= candidateCapacity)
        return SUCCESS_CODE;

    int grown = candidateCapacity ? candidateCapacity : INITIAL_RESULT_ROWS;
    while (grown < rows)
        grown *= TWO;

    int* moved = (int*)realloc(candidates, (size_t)grown * sizeof(int));
    if (moved == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    candidates        = moved;
    candidateCapacity = grown;
    return SUCCESS_CODE;
}

/*
 * One row of the build side of a hash join: its key, lifted out of the page so
 * it outlives the build scan, and where the row itself is.
 */
typedef struct {
    unsigned int hash;
    Value        key;
    int          position;
} JoinEntry;

static THREAD_LOCAL JoinEntry* joinEntries;
static THREAD_LOCAL int        joinEntryCapacity;
static THREAD_LOCAL int*       joinBuckets;          /* entry index + 1; zero means empty */
static THREAD_LOCAL int        joinBucketCount;
static THREAD_LOCAL Arena      joinKeys;             /* build-side key text */

static void freeJoinTable(void)
{
    free(joinEntries);
    free(joinBuckets);

    joinEntries       = NULL;
    joinEntryCapacity = ZERO;
    joinBuckets       = NULL;
    joinBucketCount   = ZERO;

    arenaRelease(&joinKeys);
}

/*
 * What each subquery produced. Filled once per statement, before the outer
 * query starts, which is what makes the whole feature small: by the time a row
 * is tested, a subquery is not a query any more, only a set of values.
 */
static THREAD_LOCAL ResultSet subqueryResult[MAX_SUBQUERIES];

void freeExecutor(void)
{
    freeJoinTable();

    for (int i = ZERO; i < MAX_SUBQUERIES; i++)
        freeResultSet(&subqueryResult[i]);

    free(candidates);
    candidates        = NULL;
    candidateCapacity = ZERO;

    free(probeRows);
    probeRows     = NULL;
    probeCapacity = ZERO;

    freeOrder();

    freeGroups();
    arenaRelease(&groupArena);
}

void setExplain(int on)
{
    explainEnabled = on;
}

static int resolveHavingExprs(Condition* cond, const ResultSet* out);
static int findHeader(const ResultSet* out, const char* name, int* slot);

/*
 * SQL LIKE: % matches any run of characters, _ matches exactly one.
 * Iterative with backtracking - on a mismatch it returns to the last %
 * and lets it swallow one more character. Case-sensitive, matching how
 * stored text compares everywhere else in the engine.
 */
static int likeMatch(const char* text, int length, const char* pattern)
{
    const char* afterStar = NULL;       /* pattern position just past the last % */
    const char* retry     = NULL;       /* text position that % is currently at */
    const char* end       = text + length;

    while (text != end) {
        if (*pattern == '%') {
            afterStar = ++pattern;
            retry     = text;
        }
        else if (*pattern == '_' || *pattern == *text) {
            text++;
            pattern++;
        }
        else if (afterStar != NULL) {
            pattern = afterStar;
            text    = ++retry;          /* let the % absorb one more character */
        }
        else {
            return ZERO;
        }
    }

    while (*pattern == '%')             /* trailing % can match nothing */
        pattern++;

    return *pattern == '\0';
}

/*
 * Copies the literal head of a LIKE pattern, stopping at the first wildcard,
 * and returns its length. 'abc%' gives "abc"; '%abc' gives nothing.
 */
static size_t likePrefix(const char* pattern, char* prefix)
{
    size_t n = ZERO;

    while (pattern[n] != '\0' && pattern[n] != '%' && pattern[n] != '_'
           && n < VALUE_LEN - ONE) {
        prefix[n] = pattern[n];
        n++;
    }

    prefix[n] = '\0';
    return n;
}

typedef enum { TRI_FALSE, TRI_TRUE, TRI_UNKNOWN } TriState;

/*
 * One comparison against one stored value. Returns UNKNOWN rather than FALSE
 * when NULL is involved: with AND/OR in play the difference is observable,
 * because NOT of unknown stays unknown instead of flipping to true.
 */
static TriState comparePredicate(const Value* value, const Value* other,
                                 CompareOp op)
{
    if (op == OP_IS_NULL)
        return value->isNull ? TRI_TRUE : TRI_FALSE;
    if (op == OP_IS_NOT_NULL)
        return value->isNull ? TRI_FALSE : TRI_TRUE;

    /* Either side being NULL makes the comparison unknown, which is what stops
       a join condition from pairing rows on their missing values. */
    if (value->isNull || other->isNull)
        return TRI_UNKNOWN;

    /* WHERE is type-checked in the semantic pass, HAVING cannot be: its columns
       are result labels, not catalog columns. Treat a mismatch as unknown. */
    if (value->type != other->type)
        return TRI_UNKNOWN;

    if (op == OP_LIKE)
        return likeMatch(value->text, value->textLength, valueText(other))
             ? TRI_TRUE : TRI_FALSE;
    if (op == OP_NOT_LIKE)
        return likeMatch(value->text, value->textLength, valueText(other))
             ? TRI_FALSE : TRI_TRUE;

    /* = and <> are the common case and need no ordering, so they take the
       length-first test and never reach a byte comparison unless the lengths
       already match. */
    if (op == OP_EQ)
        return valuesSame(value, other) ? TRI_TRUE : TRI_FALSE;
    if (op == OP_NE)
        return valuesSame(value, other) ? TRI_FALSE : TRI_TRUE;

    int comparison = compareValues(value, other);
    int matches;

    switch (op) {
    case OP_LT:  matches = comparison <  ZERO; break;
    case OP_LTE: matches = comparison <= ZERO; break;
    case OP_GT:  matches = comparison >  ZERO; break;
    case OP_GTE: matches = comparison >= ZERO; break;
    default:     matches = ZERO;               break;
    }

    return matches ? TRI_TRUE : TRI_FALSE;
}

/*
 * Three-valued logic over the condition tree. Note AND and OR are not
 * symmetric about UNKNOWN: false AND unknown is false, true OR unknown is true.
 */
/*
 * Both operands of a comparison are expressions now, so evaluating a condition
 * is evaluating them and comparing what comes out. An expression that cannot be
 * computed - a division by zero, a column that resolved to nothing - makes the
 * comparison UNKNOWN rather than failing the query, which is the same answer
 * NULL already gets and keeps one bad row from throwing away the rest.
 */

/*
 * x IN (values), against the one column the subquery returned.
 *
 * NULL follows SQL rather than intuition: a match is TRUE, but no match among
 * values that include a NULL is UNKNOWN rather than FALSE, because the NULL
 * might have been the match. That is why "x NOT IN (a set containing null)"
 * returns nothing at all - NOT of unknown is still unknown.
 */
static TriState valueInResult(const Value* value, const ResultSet* set)
{
    if (value->isNull)
        return TRI_UNKNOWN;

    int sawNull = ZERO;

    for (int r = ZERO; r < set->nrows; r++) {
        const Value* candidate = &set->rows[r].values[ZERO];

        if (candidate->isNull) {
            sawNull = ONE;
            continue;
        }

        if (comparePredicate(value, candidate, OP_EQ) == TRI_TRUE)
            return TRI_TRUE;
    }

    return sawNull ? TRI_UNKNOWN : TRI_FALSE;
}

static TriState evaluateCondition(const Condition* cond, int node, const Row* row)
{
    const ConditionNode* current = &cond->nodes[node];

    if (current->kind == COND_COMPARE) {
        const Predicate* compare = &current->compare;
        Value            left;
        Value            right;

        if (compare->op == OP_EXISTS) {
            const ResultSet* set = &subqueryResult[compare->subquery];
            return set->nrows > ZERO ? TRI_TRUE : TRI_FALSE;
        }

        if (exprEvaluate(&cond->exprs, compare->left, row, &left) != SUCCESS_CODE)
            return TRI_UNKNOWN;

        if (compare->op == OP_IS_NULL || compare->op == OP_IS_NOT_NULL)
            return comparePredicate(&left, &left, compare->op);

        if (compare->op == OP_IN)
            return valueInResult(&left, &subqueryResult[compare->subquery]);

        if (compare->subquery >= ZERO) {
            /* A scalar subquery that matched nothing is NULL, which is what
               SQL says and what makes the comparison unknown. More than one
               row was refused when it ran. */
            const ResultSet* set = &subqueryResult[compare->subquery];

            if (set->nrows == ZERO)
                return TRI_UNKNOWN;

            return comparePredicate(&left, &set->rows[ZERO].values[ZERO],
                                    compare->op);
        }

        if (exprEvaluate(&cond->exprs, compare->right, row, &right) != SUCCESS_CODE)
            return TRI_UNKNOWN;

        return comparePredicate(&left, &right, compare->op);
    }

    if (current->kind == COND_NOT) {
        TriState value = evaluateCondition(cond, current->left, row);
        if (value == TRI_UNKNOWN)
            return TRI_UNKNOWN;
        return value == TRI_TRUE ? TRI_FALSE : TRI_TRUE;
    }

    TriState left  = evaluateCondition(cond, current->left,  row);
    TriState right = evaluateCondition(cond, current->right, row);

    if (current->kind == COND_AND) {
        if (left == TRI_FALSE || right == TRI_FALSE)
            return TRI_FALSE;
        if (left == TRI_UNKNOWN || right == TRI_UNKNOWN)
            return TRI_UNKNOWN;
        return TRI_TRUE;
    }

    if (left == TRI_TRUE || right == TRI_TRUE)
        return TRI_TRUE;
    if (left == TRI_UNKNOWN || right == TRI_UNKNOWN)
        return TRI_UNKNOWN;
    return TRI_FALSE;
}

/*
 * Binds every column in every comparison to a position in the row.
 *
 * The semantic stage has already done this for a statement's own WHERE, and
 * doing it again costs a walk of a handful of nodes - but a condition reached
 * another way (a CHECK from the catalog, a join resolved against a schema built
 * after parsing) has not been bound to this table, and binding twice is
 * harmless where guessing which is which would not be.
 */
static int resolveCondition(const CatalogNode* table, Condition* cond)
{
    for (int i = ZERO; i < cond->count; i++) {
        if (cond->nodes[i].kind != COND_COMPARE)
            continue;

        const Predicate* compare = &cond->nodes[i].compare;

        /* EXISTS has no operands at all: it asks about a subquery, not about
           this row, so there is nothing here to bind to a column. */
        if (compare->op == OP_EXISTS)
            continue;

        int errorCode = exprResolve(table, &cond->exprs, compare->left);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        if (compare->right < ZERO)              /* IS NULL has no right side */
            continue;

        errorCode = exprResolve(table, &cond->exprs, compare->right);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }
    return SUCCESS_CODE;
}

/*
 * The deepest column any comparison reads. A scan decodes that far into each
 * record and no further, because the fields are variable-length and have to be
 * walked in order - so a condition on an early column skips most of the row.
 */
static int conditionDeepest(const Condition* cond)
{
    int deepest = -1;

    for (int i = ZERO; i < cond->count; i++) {
        if (cond->nodes[i].kind != COND_COMPARE)
            continue;

        int left  = exprDeepest(&cond->exprs, cond->nodes[i].compare.left);
        int right = exprDeepest(&cond->exprs, cond->nodes[i].compare.right);

        if (left > deepest)
            deepest = left;
        if (right > deepest)
            deepest = right;
    }

    return deepest;
}

/*
 * Picks one comparison the tree can be narrowed by. Only AND branches qualify:
 * under OR the other side could match anything, and under NOT the index would
 * return exactly the rows we do not want.
 */
static int findIndexableLeaf(const Condition* cond, int node, const CatalogNode* table)
{
    const ConditionNode* current = &cond->nodes[node];

    if (current->kind == COND_COMPARE) {
        const Predicate* compare = &current->compare;

        /*
         * A tree is seekable only from a plain column on one side and a
         * constant on the other. "a * 2 = 10" names no key the index holds -
         * it would have to be turned into "a = 5", which is algebra this
         * engine does not do - so it scans.
         */
        if (!exprIsColumn(&cond->exprs, compare->left)
            || !exprIsLiteral(&cond->exprs, compare->right))
            return -1;

        if (findIndexOn(table->table, exprColumn(&cond->exprs, compare->left)) == NULL)
            return -1;

        if (indexableOperator(compare->op))
            return node;

        if (compare->op == OP_LIKE) {
            const ExprNode* literal = &cond->exprs.nodes[compare->right];
            char            prefix[VALUE_LEN];

            if (likePrefix(valueText(&literal->literal), prefix) > ZERO)
                return node;
        }
        return -1;
    }

    if (current->kind != COND_AND)
        return -1;

    int leaf = findIndexableLeaf(cond, current->left, table);
    return leaf >= ZERO ? leaf : findIndexableLeaf(cond, current->right, table);
}

/*
 * Whether some other live row already holds this value in this column.
 *
 * A UNIQUE column always has an index behind it - CREATE TABLE makes one - so
 * this is a tree probe rather than a scan, which is what keeps a bulk load
 * into a table with a primary key from being quadratic. The index only
 * narrows: its entries can be stale, its text keys are truncated, and one of
 * them is the row being updated, so every candidate is read and compared.
 */
static int valueIsTaken(const CatalogNode* table, const Heap* heap, int slot,
                        const Value* value, int exclude, int* taken)
{
    HeapScan             scan;
    const unsigned char* record;
    Row                  row = { ZERO };
    Index*               index = findIndexOn(table->table, table->cols[slot].name);

    *taken = ZERO;

    if (index != NULL) {
        int slots = heapSlots(heap) + ONE;
        int nrows = ZERO;

        int errorCode = reserveProbe(slots);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        errorCode = indexScan(index, OP_EQ, value, probeRows, slots, &nrows);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        heapScanStart(&scan, heap);

        for (int i = ZERO; i < nrows && !*taken; i++) {
            if (probeRows[i] == exclude)        /* the row being replaced */
                continue;

            record = heapScanAt(&scan, probeRows[i]);
            if (record == NULL)                 /* tombstoned since indexing */
                continue;

            decodeRecord(record, &row, slot);

            if (!row.values[slot].isNull && valuesSame(&row.values[slot], value))
                *taken = ONE;
        }

        heapScanEnd(&scan);
        return SUCCESS_CODE;
    }

    int position;

    heapScanStart(&scan, heap);

    while (!*taken && heapScanNext(&scan, &position, &record)) {
        if (position == exclude)
            continue;

        decodeRecord(record, &row, slot);

        if (!row.values[slot].isNull && valuesSame(&row.values[slot], value))
            *taken = ONE;
    }

    heapScanEnd(&scan);
    return SUCCESS_CODE;
}

/*
 * Everything the table promises about a row, tested before the row is stored.
 * INSERT passes -1; UPDATE passes the position of the row it is replacing, so
 * that a row does not collide with the version of itself it is about to
 * replace.
 */
static int checkConstraints(const CatalogNode* table, const Heap* heap,
                            const Row* row, int exclude)
{
    for (int c = ZERO; c < table->ncols; c++) {
        const Column* column = &table->cols[c];
        const Value*  value  = &row->values[c];

        if (value->isNull) {
            if (column->flags & COL_NOT_NULL)
                return ERROR_EXEC_NOT_NULL;

            continue;               /* NULL is never equal to anything, so it
                                       cannot duplicate anything either */
        }

        if (column->type == TYPE_TEXT && column->size > ZERO
            && value->textLength > column->size)
            return ERROR_EXEC_VALUE_TOO_LONG;

        if (column->flags & COL_UNIQUE) {
            int taken;
            int errorCode = valueIsTaken(table, heap, c, value, exclude, &taken);

            if (errorCode != SUCCESS_CODE)
                return errorCode;
            if (taken)
                return ERROR_EXEC_NOT_UNIQUE;
        }
    }

    if (table->check == NULL)
        return SUCCESS_CODE;

    /* A CHECK rejects a row only when it is definitely false. A comparison
       against NULL is unknown, and SQL lets an unknown check pass - which is
       why this tests for FALSE rather than for not-TRUE. */
    return evaluateCondition(table->check, table->check->root, row)
           == TRI_FALSE ? ERROR_EXEC_CHECK_FAILED : SUCCESS_CODE;
}

/*
 * Registers the table in the catalog, gives it an empty heap, and puts an index
 * behind every column that has to stay unique.
 */
static int executeCreate(const CreateStatement* create, ResultSet* out)
{
    if (createHeap(create->table) == NULL)
        return ERROR_EXEC_TOO_MANY_TABLES;

    int errorCode = addTable(create->table, create->cols, create->ncols,
                             &create->check);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    const CatalogNode* table = findTable(create->table);

    /* A uniqueness test is a lookup, and a lookup wants a tree. Making it here
       rather than on the first insert means the promise costs the same whether
       the table is empty or already large. */
    for (int c = ZERO; c < table->ncols; c++) {
        if (!(table->cols[c].flags & COL_UNIQUE))
            continue;

        char name[NAME_LEN];

        snprintf(name, NAME_LEN, "%.24s_%.24s_key", table->table,
                 table->cols[c].name);

        if (findIndexByName(name) != NULL)
            continue;

        errorCode = createIndex(name, table->table, table->cols[c].name, c,
                                table->cols[c].type);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    snprintf(out->message, VALUE_LEN, "table %s created with %d column(s)",
             create->table, create->ncols);
    return SUCCESS_CODE;
}

/*
 * Builds an index and backfills it from the rows already in the table.
 */
static int executeCreateIndex(const CreateIndexStatement* create, ResultSet* out)
{
    const CatalogNode* table = findTable(create->table);
    const Heap*        heap  = findHeap(create->table);

    if (table == NULL || heap == NULL)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;

    int slot = findColumn(table, create->column);
    if (slot < ZERO)
        return ERROR_SEMANTIC_COLUMN_NOT_FOUND;

    int errorCode = createIndex(create->name, table->table,
                                table->cols[slot].name, slot,
                                table->cols[slot].type);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    Index* index = findIndexByName(create->name);
    Row row;

    for (int r = heapFirst(heap); r >= ZERO; r = heapNext(heap, r)) {
        ArenaMark mark = textMark();

        heapRead(heap, r, &row);
        if (!row.deleted)
            errorCode = indexInsert(index, &row.values[slot], r);

        textReset(mark);                    /* the index took its own copy */

        if (errorCode != SUCCESS_CODE) {
            /* A half-filled index answers queries with some of the rows, which
               is worse than not having one. */
            dropIndexByName(create->name);
            return errorCode;
        }
    }

    snprintf(out->message, VALUE_LEN, "index %s created on %s(%s), %d row(s) indexed",
             create->name, table->table, table->cols[slot].name, heapLive(heap));
    return SUCCESS_CODE;
}

/*
 * Appends one row to the table heap, in catalog column order,
 * then adds it to every index on that table.
 */
static int executeInsert(const InsertStatement* insert, ResultSet* out)
{
    const CatalogNode* table = findTable(insert->table);
    Heap*              heap  = findHeap(insert->table);

    if (table == NULL || heap == NULL)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;

    Row row = { ZERO };

    /* Start every column at what it would hold if the statement never
       mentioned it: its DEFAULT, or NULL. The semantic stage has already
       established that each unmentioned column will accept one of the two. */
    row.ncols = table->ncols;

    for (int c = ZERO; c < table->ncols; c++) {
        if (table->cols[c].hasDefault)
            row.values[c] = table->cols[c].defaultValue;    /* catalog-owned text */
        else
            setNull(&row.values[c], table->cols[c].type);
    }

    for (int i = ZERO; i < insert->nvalues; i++) {
        /* positional without a column list, by name with one */
        int slot = insert->ncolumns == ZERO
                 ? i : findColumn(table, insert->columns[i]);

        if (slot < ZERO)
            return ERROR_SEMANTIC_COLUMN_NOT_FOUND;

        row.values[slot] = insert->values[i];

        if (row.values[slot].isNull)                /* NULL takes the column's type */
            row.values[slot].type = table->cols[slot].type;
    }

    int rowPosition;

    int errorCode = checkConstraints(table, heap, &row, -1);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    errorCode = heapInsert(heap, &row, &rowPosition);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    errorCode = indexInsertRow(table->table, &row, rowPosition);
    if (errorCode != SUCCESS_CODE) {
        /*
         * The row is in the heap but some index does not know about it, which
         * is the one inconsistency that shows up as a wrong answer rather than
         * an error: a scan would find it and a lookup would not. Tombstoning it
         * puts the table back where the statement found it. Entries any other
         * index did take are left behind on purpose - a stale entry is safe
         * here, because every candidate is re-checked against the heap.
         */
        heapMarkDeleted(heap, rowPosition);
        return errorCode;
    }

    out->rowsAffected = ONE;
    snprintf(out->message, VALUE_LEN, "1 row inserted into %s", insert->table);
    return SUCCESS_CODE;
}

/*
 * Compacts the heap, dropping tombstoned rows, then rebuilds every index on the
 * table. Compaction moves live rows to new positions, so the rebuild is not an
 * optimisation - the old index entries would point at the wrong rows.
 */
/*
 * ALTER TABLE.
 *
 * ADD and DROP COLUMN have to rewrite every row, because a record carries its
 * own column count and its fields are variable-length: a row written before
 * the change decodes with the old shape, and there is no way to reinterpret it
 * in place. So both do what VACUUM does - rewrite, then rebuild the indexes -
 * with the transformation applied on the way through.
 *
 * The rewrite is two passes on purpose. Inserting into the heap being scanned
 * would append rows the scan then walks into, so the live positions are
 * collected first and only then rewritten.
 */
static int rewriteRows(const CatalogNode* table, Heap* heap,
                       int dropped, const Value* added)
{
    int errorCode = reserveCandidates(heapSlots(heap));
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    int* rows  = candidates;
    int  nrows = ZERO;

    for (int r = heapFirst(heap); r >= ZERO; r = heapNext(heap, r))
        rows[nrows++] = r;

    for (int i = ZERO; i < nrows; i++) {
        /* heapRead interns this row's text; heapInsert copies it into the new
           record, so the arena can be wound back before the next one. */
        ArenaMark mark = textMark();
        Row       row;

        errorCode = heapRead(heap, rows[i], &row);
        if (errorCode != SUCCESS_CODE) {
            textReset(mark);
            return errorCode;
        }

        if (row.deleted) {
            textReset(mark);
            continue;
        }

        if (dropped >= ZERO) {
            for (int c = dropped; c + ONE < row.ncols; c++)
                row.values[c] = row.values[c + ONE];
            row.ncols--;
        }
        else {
            /* A row written before the column existed simply has fewer fields
               than the table now has, so the new value goes at the end. */
            row.values[row.ncols++] = *added;
        }

        int landed;
        errorCode = heapInsert(heap, &row, &landed);
        if (errorCode == SUCCESS_CODE)
            errorCode = heapMarkDeleted(heap, rows[i]);

        textReset(mark);

        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    int reclaimed;
    errorCode = heapCompact(heap, &reclaimed);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    return rebuildIndexes(table->table, heap);
}

static int executeAlter(const AlterStatement* alter, ResultSet* out)
{
    CatalogNode* table = findTable(alter->table);
    Heap*        heap  = findHeap(alter->table);

    if (table == NULL || heap == NULL)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;

    switch (alter->action) {
    case ALTER_ADD_COLUMN: {
        if (findColumn(table, alter->column.name) >= ZERO)
            return ERROR_SEMANTIC_DUPLICATE_COLUMN;

        if (table->ncols == MAX_COLS)
            return ERROR_EXEC_TABLE_TOO_WIDE;

        /* What every existing row gets. NOT NULL with rows already in the
           table needs a DEFAULT to be satisfiable, which the semantic stage
           has already insisted on. */
        Value filled;

        if (alter->column.hasDefault) {
            filled = alter->column.defaultValue;
        }
        else {
            memset(&filled, ZERO, sizeof filled);
            filled.isNull = ONE;
            filled.type   = alter->column.type;
        }

        table->cols[table->ncols++] = alter->column;

        int errorCode = rewriteRows(table, heap, -ONE, &filled);
        if (errorCode != SUCCESS_CODE) {
            table->ncols--;                     /* put the catalog back */
            return errorCode;
        }

        snprintf(out->message, VALUE_LEN, "%s: column %s added",
                 table->table, alter->column.name);
        return SUCCESS_CODE;
    }

    case ALTER_DROP_COLUMN: {
        int slot = findColumn(table, alter->name);

        if (slot < ZERO)
            return ERROR_SEMANTIC_COLUMN_NOT_FOUND;
        if (table->ncols == ONE)
            return ERROR_SEMANTIC_LAST_COLUMN;

        /* The trees on this column go, and every tree on a later column is
           now reading a slot that has shifted down. Done before the rewrite,
           so the rebuild that ends it fills the corrected slots. */
        indexesColumnDropped(table->table, table->cols[slot].name, slot);

        for (int c = slot; c + ONE < table->ncols; c++)
            table->cols[c] = table->cols[c + ONE];
        table->ncols--;

        int errorCode = rewriteRows(table, heap, slot, NULL);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        snprintf(out->message, VALUE_LEN, "%s: column %s dropped",
                 table->table, alter->name);
        return SUCCESS_CODE;
    }

    case ALTER_RENAME_COLUMN: {
        int slot = findColumn(table, alter->name);

        if (slot < ZERO)
            return ERROR_SEMANTIC_COLUMN_NOT_FOUND;
        if (findColumn(table, alter->newName) >= ZERO)
            return ERROR_SEMANTIC_DUPLICATE_COLUMN;

        /* Names only: a record stores values in slot order and says nothing
           about what they are called, so no row is touched. */
        indexesColumnRenamed(table->table, table->cols[slot].name, alter->newName);
        snprintf(table->cols[slot].name, NAME_LEN, "%s", alter->newName);

        snprintf(out->message, VALUE_LEN, "%s: column %s renamed to %s",
                 table->table, alter->name, alter->newName);
        return SUCCESS_CODE;
    }

    case ALTER_RENAME_TABLE: {
        char was[NAME_LEN];
        snprintf(was, NAME_LEN, "%s", table->table);

        int errorCode = renameTableInCatalog(was, alter->newName);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        renameHeap(was, alter->newName);
        indexesTableRenamed(was, alter->newName);

        snprintf(out->message, VALUE_LEN, "table %s renamed to %s",
                 was, alter->newName);
        return SUCCESS_CODE;
    }
    }

    return ERROR_SYNTAX_INVALID_STATEMENT;
}

static int executeVacuum(const char* tableName, ResultSet* out)
{
    const CatalogNode* table = findTable(tableName);
    Heap*              heap  = findHeap(tableName);

    if (table == NULL || heap == NULL)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;

    int slotsBefore = heapSlots(heap);
    int live        = ZERO;

    int reclaimed;
    int compactError = heapCompact(heap, &reclaimed);
    if (compactError != SUCCESS_CODE)
        return compactError;

    live = heapLive(heap);

    int errorCode = rebuildIndexes(table->table, heap);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    snprintf(out->message, VALUE_LEN,
             "%s: %d slot(s) compacted to %d row(s), %d reclaimed",
             table->table, slotsBefore, live, slotsBefore - live);
    return SUCCESS_CODE;
}

/*
 * Chooses the access path
 and returns the heap positions that satisfy WHERE.
 * An index answers equality and ranges; everything else is a full scan.
 */
static int collectRows(const CatalogNode* table, const Heap* heap,
                       Condition* where, int* rows, int* nrows, int stopAt)
{
    int                  slots = heapSlots(heap);
    HeapScan             scan;
    int                  position;
    const unsigned char* record;

    /* Nothing to test, so nothing is decoded: the positions are the answer. */
    if (!where->present) {
        if (explainEnabled)
            printf("[table scan: %s, %d slot(s)]\n", table->table, slots);

        *nrows = ZERO;
        heapScanStart(&scan, heap);

        while (heapScanNext(&scan, &position, &record)) {
            rows[(*nrows)++] = position;

            if (*nrows == stopAt)                   /* LIMIT, and nothing above
                                                       this reorders the rows */
                break;
        }

        heapScanEnd(&scan);
        return SUCCESS_CODE;
    }

    int errorCode = resolveCondition(table, where);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /* Rows are tested where they lie. Nothing is copied out and nothing is
       interned, because a row that fails the condition leaves only its
       position behind - and a row that passes is read again by the projection,
       which is the pass that has to keep anything. */
    int upto = conditionDeepest(where);
    Row row  = { ZERO };

    int leaf = findIndexableLeaf(where, where->root, table);

    if (leaf >= ZERO) {
        const Predicate* predicate = &where->nodes[leaf].compare;
        const Value*     key       = &where->exprs.nodes[predicate->right].literal;
        Index*           index     = findIndexOn(table->table,
                                                 exprColumn(&where->exprs,
                                                            predicate->left));

        if (predicate->op == OP_LIKE) {
            char prefix[VALUE_LEN];
            likePrefix(valueText(key), prefix);

            if (explainEnabled)
                printf("[index prefix scan: %s on %s(%s), prefix \"%s\"]\n",
                       index->name, index->table, index->column, prefix);

            errorCode = indexPrefixScan(index, prefix, rows, slots, nrows);
        }
        else {
            if (explainEnabled)
                printf("[index scan: %s on %s(%s)]\n",
                       index->name, index->table, index->column);

            errorCode = indexScan(index, predicate->op, key, rows, slots, nrows);
        }

        if (errorCode != SUCCESS_CODE)
            return errorCode;

        /* the index only narrows candidates: every row still gets the full test */
        int kept = ZERO;

        heapScanStart(&scan, heap);

        for (int i = ZERO; i < *nrows; i++) {
            record = heapScanAt(&scan, rows[i]);

            if (record == NULL)                 /* tombstoned since it was indexed */
                continue;

            decodeRecord(record, &row, upto);

            if (evaluateCondition(where, where->root, &row) == TRI_TRUE) {
                rows[kept++] = rows[i];

                if (kept == stopAt)
                    break;
            }
        }

        heapScanEnd(&scan);
        *nrows = kept;

        return SUCCESS_CODE;
    }

    if (explainEnabled)
        printf("[table scan: %s, %d slot(s)]\n", table->table, slots);

    *nrows = ZERO;
    heapScanStart(&scan, heap);

    while (heapScanNext(&scan, &position, &record)) {
        decodeRecord(record, &row, upto);

        if (evaluateCondition(where, where->root, &row) == TRI_TRUE) {
            rows[(*nrows)++] = position;

            if (*nrows == stopAt)
                break;
        }
    }

    heapScanEnd(&scan);
    return SUCCESS_CODE;
}

/*
 * Puts the statement's SET values into a row read from the heap.
 *
 * Each one is an expression over that same row, which is what "set balance =
 * balance * 1.1" means: the old row is read, the new values are computed from
 * it, and only then is anything stored.
 */
static int applyAssignments(const CatalogNode* table, const UpdateStatement* update,
                            const int* setSlot, Row* row)
{
    Value computed[MAX_COLS];

    /* Every assignment sees the row as it was, so "set a = b, b = a" swaps
       rather than copying one into the other twice. */
    for (int c = ZERO; c < update->nsets; c++) {
        int errorCode = exprEvaluate(&update->exprs, update->sets[c].expr,
                                     row, &computed[c]);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        /* An int assigned to a float column is that number, not a mismatch. */
        if (!computed[c].isNull
            && computed[c].type == TYPE_INT
            && table->cols[setSlot[c]].type == TYPE_FLOAT)
            setFloat(&computed[c], (double)computed[c].intValue);

        if (!computed[c].isNull
            && computed[c].type != table->cols[setSlot[c]].type)
            return ERROR_SEMANTIC_TYPE_MISMATCH;
    }

    for (int c = ZERO; c < update->nsets; c++) {
        row->values[setSlot[c]] = computed[c];

        if (row->values[setSlot[c]].isNull)         /* NULL takes the column type */
            row->values[setSlot[c]].type = table->cols[setSlot[c]].type;
    }

    row->deleted = ZERO;
    return SUCCESS_CODE;
}

/*
 * Writes a new version of each matching row and tombstones the old one, exactly
 * as DELETE + INSERT would. Updating in place would strand index entries under
 * the old key; appending a new version means the existing index maintenance and
 * tombstone filtering handle it with no special case.
 * Every update burns a slot; VACUUM reclaims them.
 */
static int executeUpdate(UpdateStatement* update, ResultSet* out)
{
    const CatalogNode* table = findTable(update->table);
    Heap*              heap  = findHeap(update->table);

    if (table == NULL || heap == NULL)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;

    int setSlot[MAX_COLS];
    for (int i = ZERO; i < update->nsets; i++) {
        setSlot[i] = findColumn(table, update->sets[i].column);
        if (setSlot[i] < ZERO)
            return ERROR_SEMANTIC_COLUMN_NOT_FOUND;
    }

    int errorCode = reserveCandidates(heapSlots(heap));
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    int* rows  = candidates;
    int  nrows = ZERO;

    errorCode = collectRows(table, heap, &update->where, rows, &nrows, -1);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /*
     * Every row is checked before any row is written.
     *
     * The alternative is finding out at row fifty that the statement cannot
     * finish, with forty-nine rows already changed and no way to put them
     * back - a statement either happens or it does not, and there is no undo
     * short of a transaction. The pass costs a second read of the rows being
     * updated, which is the cheapest part of an UPDATE.
     */
    for (int i = ZERO; i < nrows; i++) {
        ArenaMark mark = textMark();
        Row       updated;

        heapRead(heap, rows[i], &updated);

        errorCode = applyAssignments(table, update, setSlot, &updated);

        /* The row being replaced is not competition for itself, so it is
           excluded from the uniqueness probe. */
        if (errorCode == SUCCESS_CODE)
            errorCode = checkConstraints(table, heap, &updated, rows[i]);

        textReset(mark);

        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    /*
     * One more thing the pass above cannot see: the rows are all being given
     * the same literal, so a unique column and more than one row is a
     * collision between the new rows themselves, which no probe against the
     * stored rows would report.
     */
    if (nrows > ONE)
        for (int c = ZERO; c < update->nsets; c++) {
            const Value* literal = exprLiteral((ExprPool*)&update->exprs,
                                               update->sets[c].expr);

            /* Only a literal is the same for every row. "set id = id + 1"
               gives each row a different value, so it may well be fine. */
            if ((table->cols[setSlot[c]].flags & COL_UNIQUE)
                && literal != NULL && !literal->isNull)
                return ERROR_EXEC_NOT_UNIQUE;
        }

    for (int i = ZERO; i < nrows; i++) {
        ArenaMark mark = textMark();
        Row       updated;

        heapRead(heap, rows[i], &updated);

        errorCode = applyAssignments(table, update, setSlot, &updated);
        if (errorCode != SUCCESS_CODE) {
            textReset(mark);
            return errorCode;
        }

        int newPosition;

        /* store first: a failure here must leave the original row untouched */
        errorCode = heapInsert(heap, &updated, &newPosition);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        /*
         * And index it before the old row is retired, so that a failure here
         * can still be undone: the new row is tombstoned and the old one is
         * exactly where it was. Retiring the old one first would leave the
         * row with no live version at all.
         */
        errorCode = indexInsertRow(table->table, &updated, newPosition);
        if (errorCode != SUCCESS_CODE) {
            heapMarkDeleted(heap, newPosition);
            return errorCode;
        }

        heapMarkDeleted(heap, rows[i]);

        textReset(mark);                    /* the page and the index hold it now */
    }

    out->rowsAffected = nrows;
    snprintf(out->message, VALUE_LEN, "%d row(s) updated in %s", nrows, table->table);
    return SUCCESS_CODE;
}

/*
 * Marks matching rows as deleted.
 Rows keep their slot so that index entries,
 * which store slot positions, stay valid.
 * Space is not reclaimed until VACUUM runs.
 */
static int executeDelete(DeleteStatement* del, ResultSet* out)
{
    const CatalogNode* table = findTable(del->table);
    Heap*              heap  = findHeap(del->table);

    if (table == NULL || heap == NULL)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;

    int errorCode = reserveCandidates(heapSlots(heap));
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    int* rows  = candidates;
    int  nrows = ZERO;

    errorCode = collectRows(table, heap, &del->where, rows, &nrows, -1);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    for (int i = ZERO; i < nrows; i++)
        heapMarkDeleted(heap, rows[i]);

    out->rowsAffected = nrows;
    snprintf(out->message, VALUE_LEN, "%d row(s) deleted from %s", nrows, table->table);
    return SUCCESS_CODE;
}

/*
 * Plain projection: no GROUP BY, no aggregates. One output row per input row.
 */
static int projectRows(SelectStatement* select, const CatalogNode* table,
                       const Heap* heap, const int* rows, int nrows, ResultSet* out)
{
    int slot[MAX_COLS];                 /* result column -> position in the row */

    if (select->selectAll) {
        out->ncols = table->ncols;
        for (int c = ZERO; c < table->ncols; c++) {
            slot[c]     = c;
            out->types[c] = table->cols[c].type;
            snprintf(out->headers[c], NAME_LEN, "%s", table->cols[c].name);
        }
    }
    else {
        out->ncols = select->nitems;
        for (int c = ZERO; c < select->nitems; c++) {
            const SelectItem* item = &select->items[c];

            slot[c] = -1;

            int errorCode = exprResolve(table, &select->exprs, item->expr);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            errorCode = exprType(&select->exprs, item->expr, &out->types[c]);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            /* Unaliased, a bare column keeps the column's own name rather than
               how the query spelled it - "select users.name from users" still
               says "name" - and anything computed is named after what it says. */
            if (!item->aliased && exprIsColumn(&select->exprs, item->expr)) {
                slot[c] = findColumn(table, exprColumn(&select->exprs, item->expr));
                if (slot[c] < ZERO)
                    return ERROR_SEMANTIC_COLUMN_NOT_FOUND;

                snprintf(out->headers[c], NAME_LEN, "%s", table->cols[slot[c]].name);
            }
            else {
                snprintf(out->headers[c], NAME_LEN, "%s", item->label);
            }
        }
    }

    int errorCode = resultReserve(out, nrows);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /* Only the columns being projected are decoded, and only their text is
       copied out of the page. Selecting one column of a wide row no longer
       unpacks - or interns - the columns nobody asked for. */
    int deepest = -1;

    if (select->selectAll) {
        deepest = table->ncols - ONE;
    }
    else {
        for (int c = ZERO; c < select->nitems; c++) {
            int at = exprDeepest(&select->exprs, select->items[c].expr);

            if (at > deepest)
                deepest = at;
        }
    }

    HeapScan             scan;
    const unsigned char* record;
    Row                  source = { ZERO };

    heapScanStart(&scan, heap);

    out->nrows = ZERO;
    for (int i = ZERO; i < nrows; i++) {
        record = heapScanAt(&scan, rows[i]);
        if (record == NULL)
            continue;

        decodeRecord(record, &source, deepest);

        Row* target = &out->rows[out->nrows];

        for (int c = ZERO; c < out->ncols; c++) {
            Value value;

            if (select->selectAll) {
                value = source.values[c];
            }
            else {
                int errorCode = exprEvaluate(&select->exprs,
                                             select->items[c].expr,
                                             &source, &value);
                if (errorCode != SUCCESS_CODE) {
                    heapScanEnd(&scan);
                    return errorCode;
                }
            }

            /* the result outlives the pin, so its text cannot stay in the page */
            if (!value.isNull && value.type == TYPE_TEXT)
                setText(&value, value.text, value.textLength);

            target->values[c] = value;
        }

        target->ncols = out->ncols;
        out->nrows++;
    }

    heapScanEnd(&scan);
    return SUCCESS_CODE;
}

/*
 * Finds the group this row belongs to, or -1. With no GROUP BY there is a
 * single group, so every row matches the one that already exists.
 */
/*
 * The group this row belongs to, or -1. With no GROUP BY every row hashes the
 * same and matches on zero columns, so there is a single group - which is what
 * an aggregate with no grouping wants.
 */
static int findGroup(const Row* row, const int* groupSlots, int ngroup,
                     unsigned int hash)
{
    if (bucketCount == ZERO)
        return -1;

    unsigned int mask = (unsigned int)bucketCount - ONE;

    for (unsigned int probe = hash & mask; ; probe = (probe + ONE) & mask) {
        int slot = groupBuckets[probe];

        if (slot == ZERO)
            return -1;

        int  group = slot - ONE;
        int  same  = ONE;

        for (int k = ZERO; k < ngroup && same; k++)
            if (!valuesEqual(&row->values[groupSlots[k]], keyAt(group, k)))
                same = ZERO;

        if (same)
            return group;
    }
}

/* Reinserts every existing group into a fresh, larger bucket array, using the
   hash each one was found by rather than working it out again. */
static int rehashGroups(int ngroups)
{
    int  grown = bucketCount ? bucketCount * TWO : 256;
    int* moved = (int*)calloc((size_t)grown, sizeof(int));

    if (moved == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    free(groupBuckets);
    groupBuckets = moved;
    bucketCount  = grown;

    unsigned int mask = (unsigned int)grown - ONE;

    for (int g = ZERO; g < ngroups; g++) {
        unsigned int probe = groupHashes[g] & mask;

        while (groupBuckets[probe] != ZERO)
            probe = (probe + ONE) & mask;

        groupBuckets[probe] = g + ONE;
    }

    return SUCCESS_CODE;
}

/* Room for one more group, in both the key table and the bucket array. */
static int reserveGroup(int ngroups)
{
    if (ngroups == groupCapacity) {
        int grown = groupCapacity ? groupCapacity * TWO : 256;

        Value* keys = (Value*)realloc(groupKeys,
                          (size_t)grown * groupKeyStride * sizeof(Value));
        if (keys == NULL)
            return ERROR_EXEC_OUT_OF_MEMORY;
        groupKeys = keys;

        Accumulator* accs = (Accumulator*)realloc(accumulators,
                                (size_t)grown * groupAccStride * sizeof(Accumulator));
        if (accs == NULL)
            return ERROR_EXEC_OUT_OF_MEMORY;
        accumulators = accs;

        unsigned int* hashes = (unsigned int*)realloc(groupHashes,
                                   (size_t)grown * sizeof(unsigned int));
        if (hashes == NULL)
            return ERROR_EXEC_OUT_OF_MEMORY;

        groupHashes   = hashes;
        groupCapacity = grown;
    }

    /* keep the table under two thirds full so probes stay short */
    if ((ngroups + ONE) * 3 >= bucketCount * TWO)
        return rehashGroups(ngroups);

    return SUCCESS_CODE;
}

static void recordGroup(int group, unsigned int hash)
{
    unsigned int mask  = (unsigned int)bucketCount - ONE;
    unsigned int probe = hash & mask;

    while (groupBuckets[probe] != ZERO)
        probe = (probe + ONE) & mask;

    groupBuckets[probe] = group + ONE;
}

static void accumulate(Accumulator* acc, const SelectItem* item, const Value* value)
{
    if (item->aggregate == AGG_NONE)
        return;

    if (item->star) {                   /* count(*) counts rows, NULLs included */
        acc->count++;
        acc->seen = ONE;
        return;
    }

    if (value->isNull)                  /* every other aggregate skips NULLs */
        return;

    acc->count++;

    if (item->aggregate == AGG_SUM || item->aggregate == AGG_AVG) {
        /* Both totals are kept: sum answers exactly in the column's own type,
           avg divides and is a real number whatever it was summing. */
        if (value->type == TYPE_FLOAT) {
            acc->real += value->floatValue;
        }
        else {
            acc->sum  += value->intValue;
            acc->real += (double)value->intValue;
        }
    }
    else if (item->aggregate == AGG_MIN || item->aggregate == AGG_MAX) {
        int wantLower = (item->aggregate == AGG_MIN);
        if (!acc->seen
            || (wantLower  && compareValues(value, &acc->extreme) < ZERO)
            || (!wantLower && compareValues(value, &acc->extreme) > ZERO))
            retain(&acc->extreme, value);
    }

    acc->seen = ONE;
}

/*
 * GROUP BY and/or aggregates: one output row per group.
 */
static int projectGroups(SelectStatement* select, const CatalogNode* table,
                         const Heap* heap, const int* rows, int nrows, ResultSet* out)
{
    int groupSlots[MAX_COLS];
    for (int k = ZERO; k < select->ngroup; k++) {
        groupSlots[k] = findColumn(table, select->groupBy[k]);
        if (groupSlots[k] < ZERO)
            return ERROR_SEMANTIC_COLUMN_NOT_FOUND;
    }

    int     itemKey[MAX_COLS];          /* plain column -> its GROUP BY position */
    ColType itemType[MAX_COLS];         /* what the item's expression produces */

    for (int i = ZERO; i < select->nitems; i++) {
        const SelectItem* item = &select->items[i];

        itemKey[i]  = -1;
        itemType[i] = TYPE_INT;

        if (item->star)                 /* count(*) reads nothing */
            continue;

        int errorCode = exprResolve(table, &select->exprs, item->expr);

        if (errorCode == SUCCESS_CODE)
            errorCode = exprType(&select->exprs, item->expr, &itemType[i]);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        if (item->aggregate == AGG_NONE
            && exprIsColumn(&select->exprs, item->expr))
            for (int k = ZERO; k < select->ngroup; k++)
                if (_stricmp(select->groupBy[k],
                             exprColumn(&select->exprs, item->expr)) == ZERO)
                    itemKey[i] = k;
    }

    int ngroups = ZERO;

    /* One row of the group table is exactly what this query needs, no more. */
    groupKeyStride = select->ngroup > ZERO ? select->ngroup : ONE;
    groupAccStride = select->nitems > ZERO ? select->nitems : ONE;
    groupCapacity  = ZERO;                  /* the stride changed, so the old
                                               table is the wrong shape */

    arenaRelease(&groupArena);              /* last query's keys are done with */

    if (bucketCount > ZERO)
        memset(groupBuckets, ZERO, (size_t)bucketCount * sizeof(int));

    /* Grouping reads rows where they lie: a key or a running min/max is copied
       into the group arena by retain(), and everything else is looked at and
       left behind. Nothing reaches the statement arena, so there is nothing to
       wind back after each row. */
    int deepest = -1;
    for (int k = ZERO; k < select->ngroup; k++)
        if (groupSlots[k] > deepest)
            deepest = groupSlots[k];
    for (int c = ZERO; c < select->nitems; c++) {
        int at = exprDeepest(&select->exprs, select->items[c].expr);

        if (at > deepest)
            deepest = at;
    }

    /*
     * count(*) with no grouping names no column at all, so there is nothing to
     * read: the answer is how many rows the scan kept. This is what makes
     * "select count(*) from t" cost the walk over the slot directories and
     * nothing else.
     */
    if (deepest < ZERO && nrows > ZERO) {
        int errorCode = reserveGroup(ZERO);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        ngroups = ONE;
        for (int c = ZERO; c < select->nitems; c++) {
            Accumulator* acc = accumulatorAt(ZERO, c);

            acc->count = nrows;
            acc->sum   = ZERO;
            acc->real  = 0.0;
            acc->seen  = ONE;
        }

        nrows = ZERO;                       /* the loop below has nothing to do */
    }

    HeapScan             scan;
    const unsigned char* record;
    Row                  rowStorage = { ZERO };
    const Row*           row        = &rowStorage;

    heapScanStart(&scan, heap);

    for (int i = ZERO; i < nrows; i++) {
        record = heapScanAt(&scan, rows[i]);
        if (record == NULL)
            continue;

        decodeRecord(record, &rowStorage, deepest);

        unsigned int hash = hashKey(row, groupSlots, select->ngroup);
        int          g    = findGroup(row, groupSlots, select->ngroup, hash);

        if (g < ZERO) {
            int errorCode = reserveGroup(ngroups);
            if (errorCode != SUCCESS_CODE) {
                heapScanEnd(&scan);
                return errorCode;
            }

            g = ngroups++;
            groupHashes[g] = hash;
            recordGroup(g, hash);

            for (int k = ZERO; k < select->ngroup; k++)
                retain(keyAt(g, k), &row->values[groupSlots[k]]);
            for (int c = ZERO; c < select->nitems; c++) {
                accumulatorAt(g, c)->count = ZERO;
                accumulatorAt(g, c)->sum   = ZERO;
                accumulatorAt(g, c)->real  = 0.0;
                accumulatorAt(g, c)->seen  = ZERO;
            }
        }

        for (int c = ZERO; c < select->nitems; c++) {
            Value computed;

            /* count(*) names nothing to compute, and accumulate knows it */
            if (select->items[c].star) {
                accumulate(accumulatorAt(g, c), &select->items[c], NULL);
                continue;
            }

            int errorCode = exprEvaluate(&select->exprs, select->items[c].expr,
                                         row, &computed);
            if (errorCode != SUCCESS_CODE) {
                heapScanEnd(&scan);
                return errorCode;
            }

            accumulate(accumulatorAt(g, c), &select->items[c], &computed);
        }
    }

    heapScanEnd(&scan);

    /* An aggregate over no rows still returns one row, but only without GROUP BY */
    if (ngroups == ZERO && select->ngroup == ZERO) {
        int errorCode = reserveGroup(ZERO);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        ngroups = ONE;
        for (int c = ZERO; c < select->nitems; c++) {
            accumulatorAt(ZERO, c)->count = ZERO;
            accumulatorAt(ZERO, c)->sum   = ZERO;
            accumulatorAt(ZERO, c)->real  = 0.0;
            accumulatorAt(ZERO, c)->seen  = ZERO;
        }
    }

    out->ncols = select->nitems;
    for (int i = ZERO; i < select->nitems; i++) {
        const SelectItem* item = &select->items[i];

        /* count is a number of rows, avg divides, and the rest answer in
           whatever they were given. */
        out->types[i] = item->aggregate == AGG_COUNT ? TYPE_INT
                      : item->aggregate == AGG_AVG   ? TYPE_FLOAT
                      : itemType[i];

        if (item->aggregate == AGG_NONE && !item->aliased
            && exprIsColumn(&select->exprs, item->expr)) {
            int at = findColumn(table, exprColumn(&select->exprs, item->expr));

            snprintf(out->headers[i], NAME_LEN, "%s",
                     at >= ZERO ? table->cols[at].name : item->label);
        }
        else {
            snprintf(out->headers[i], NAME_LEN, "%s", item->label);
        }
    }

    if (select->having.present) {
        int errorCode = resolveHavingExprs(&select->having, out);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    int reserveError = resultReserve(out, ngroups);
    if (reserveError != SUCCESS_CODE)
        return reserveError;

    out->nrows = ZERO;
    for (int g = ZERO; g < ngroups; g++) {
        Row* row = &out->rows[out->nrows];

        for (int i = ZERO; i < select->nitems; i++) {
            const Accumulator* acc   = accumulatorAt(g, i);
            const SelectItem*  item  = &select->items[i];
            Value*             value = &row->values[i];

            switch (item->aggregate) {
            case AGG_NONE:
                *value = *keyAt(g, itemKey[i]);
                break;

            case AGG_COUNT:
                value->isNull   = ZERO;
                value->type     = TYPE_INT;
                value->intValue = acc->count;
                break;

            case AGG_AVG:
                /* No rows contributed, so there is nothing to divide by and
                   the average is unknown rather than zero. */
                if (!acc->seen || acc->count == ZERO)
                    setNull(value, TYPE_FLOAT);
                else
                    setFloat(value, acc->real / (double)acc->count);
                break;

            case AGG_SUM:
                if (!acc->seen) {       /* nothing non-NULL to add: SUM is NULL */
                    setNull(value, itemType[i]);
                    break;
                }

                /* a total over floats is a float, not a rounded int */
                if (itemType[i] == TYPE_FLOAT) {
                    setFloat(value, acc->real);
                    break;
                }
                if (acc->sum < INT_MIN || acc->sum > INT_MAX)
                    return ERROR_VALUE_OUT_OF_RANGE;
                value->isNull   = ZERO;
                value->type     = TYPE_INT;
                value->intValue = (int)acc->sum;
                break;

            default:                    /* AGG_MIN / AGG_MAX */
                if (acc->seen)
                    *value = acc->extreme;
                else
                    setNull(value, itemType[i]);
                break;
            }
        }

        row->ncols = select->nitems;

        /* HAVING drops whole groups, so the row is built and then discarded */
        if (select->having.present
            && evaluateCondition(&select->having, select->having.root,
                                 row) != TRI_TRUE)
            continue;

        out->nrows++;
    }

    return SUCCESS_CODE;
}

/* Kept rows, by hash, for DISTINCT. Holds position + 1, so zero means empty. */
static THREAD_LOCAL int* distinctBuckets;
static THREAD_LOCAL int  distinctCount;

/* qsort takes no context pointer, so the sort keys live here. Single-threaded. */
static THREAD_LOCAL int sortSlot[MAX_COLS];
static THREAD_LOCAL int sortDescending[MAX_COLS];
static THREAD_LOCAL int sortTerms;

/*
 * Sorting works on a small array beside the rows rather than on the rows.
 *
 * Two things are wrong with sorting Rows directly. Each one is over half a
 * kilobyte, so every swap moves 1.5 KB; and the comparator only ever looks at
 * one or two values in it, so each comparison touches a cache line of a
 * megabytes-wide array to read 32 bytes.
 *
 * A SortKey carries the first ORDER BY value inline - which decides almost
 * every comparison - and the row it came from. Later terms are rare and read
 * the row itself, which is exactly when the extra reach is affordable.
 */
typedef struct {
    Value first;
    int   index;
} SortKey;

static THREAD_LOCAL const Row* sortRows;
static THREAD_LOCAL SortKey*   sortKeys;
static THREAD_LOCAL int        sortCapacity;

static int reserveOrder(int rows)
{
    if (rows <= sortCapacity)
        return SUCCESS_CODE;

    int grown = sortCapacity ? sortCapacity : INITIAL_RESULT_ROWS;
    while (grown < rows)
        grown *= TWO;

    SortKey* moved = (SortKey*)realloc(sortKeys, (size_t)grown * sizeof(SortKey));
    if (moved == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    sortKeys     = moved;
    sortCapacity = grown;
    return SUCCESS_CODE;
}

/*
 * Ordering comparison, which is not the same as the WHERE comparison:
 * here NULL is a sortable value that comes first, rather than an unknown.
 */
static int orderCompare(const Value* a, const Value* b)
{
    if (a->isNull || b->isNull) {
        if (a->isNull && b->isNull)
            return ZERO;
        return a->isNull ? -1 : ONE;
    }
    return compareValues(a, b);
}

static int sortCompare(const SortKey* a, const SortKey* b)
{
    int comparison = orderCompare(&a->first, &b->first);

    if (comparison != ZERO)
        return sortDescending[ZERO] ? -comparison : comparison;

    /* The first term tied, so the rest of it has to be looked up. */
    for (int i = ONE; i < sortTerms; i++) {
        const Value* left4  = &sortRows[a->index].values[sortSlot[i]];
        const Value* right4 = &sortRows[b->index].values[sortSlot[i]];

        comparison = orderCompare(left4, right4);
        if (comparison != ZERO)
            return sortDescending[i] ? -comparison : comparison;
    }

    /* qsort is not stable, so ties fall back to the order the rows arrived in.
       Two runs of the same query then print the same thing. */
    return a->index < b->index ? -1 : a->index > b->index ? ONE : ZERO;
}

static int compareResultRows(const void* left, const void* right)
{
    return sortCompare((const SortKey*)left, (const SortKey*)right);
}

static int compareByIndex(const void* left, const void* right)
{
    int a = ((const SortKey*)left)->index;
    int b = ((const SortKey*)right)->index;

    return a < b ? -1 : a > b ? ONE : ZERO;
}

/* Pushes keys[at] down until the subtree below it is a heap again. */
static void siftDown(SortKey* keys, int count, int at)
{
    for (;;) {
        int worst = at;
        int left  = at * TWO + ONE;
        int right = left + ONE;

        if (left < count && sortCompare(&keys[left], &keys[worst]) > ZERO)
            worst = left;
        if (right < count && sortCompare(&keys[right], &keys[worst]) > ZERO)
            worst = right;

        if (worst == at)
            return;

        SortKey held = keys[at];

        keys[at]    = keys[worst];
        keys[worst] = held;
        at          = worst;
    }
}

/*
 * ORDER BY with a LIMIT does not need the rows it is going to throw away put in
 * order. Keeping the best k in a heap whose root is the worst of them costs one
 * comparison per row that cannot get in, and log k for one that can - against
 * the log n per row a full sort spends on every row including the discarded
 * ones. Ordering fifty thousand rows to print five went from 745,000
 * comparisons to about 50,000.
 *
 * The k that survive are then compacted to the front, so the sort and the
 * shuffle that follow work on k rows rather than n.
 */
static void selectTop(ResultSet* out, int wanted)
{
    int n = out->nrows;

    for (int i = wanted / TWO - ONE; i >= ZERO; i--)
        siftDown(sortKeys, wanted, i);

    for (int i = wanted; i < n; i++) {
        if (sortCompare(&sortKeys[i], &sortKeys[ZERO]) >= ZERO)
            continue;                       /* no better than the worst kept */

        sortKeys[ZERO] = sortKeys[i];
        siftDown(sortKeys, wanted, ZERO);
    }

    /* Moving them forward in the order they already lie in means a row is never
       overwritten before it has been read: the jth kept row can only have come
       from position j or later. */
    qsort(sortKeys, (size_t)wanted, sizeof(SortKey), compareByIndex);

    for (int j = ZERO; j < wanted; j++) {
        if (sortKeys[j].index != j)
            out->rows[j] = out->rows[sortKeys[j].index];

        sortKeys[j].index = j;
    }

    out->nrows = wanted;
}

/*
 * ORDER BY runs last, over the finished result set, so one implementation
 * serves plain projections and grouped output alike. Terms name output
 * columns, matched against the headers.
 */
static int applyOrder(const SelectStatement* select, ResultSet* out)
{
    if (select->norder == ZERO)
        return SUCCESS_CODE;

    sortTerms = select->norder;
    for (int i = ZERO; i < select->norder; i++) {
        int errorCode = findHeader(out, select->order[i].column, &sortSlot[i]);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        sortDescending[i] = select->order[i].descending;
    }

    int errorCode = reserveOrder(out->nrows);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    for (int i = ZERO; i < out->nrows; i++) {
        sortKeys[i].first = out->rows[i].values[sortSlot[ZERO]];
        sortKeys[i].index = i;
    }

    sortRows = out->rows;

    /* A LIMIT smaller than the result means most of these rows are only being
       ordered to be discarded. */
    if (select->limit >= ZERO && select->limit < out->nrows) {
        if (select->limit == ZERO) {
            out->nrows = ZERO;
            return SUCCESS_CODE;
        }

        selectTop(out, select->limit);
    }

    qsort(sortKeys, (size_t)out->nrows, sizeof(SortKey), compareResultRows);

    /* Now put the rows in that order, moving each one once. Following each
       cycle to its end is what keeps it to one move and one spare Row, rather
       than a second copy of a result set that may be hundreds of megabytes. */
    for (int i = ZERO; i < out->nrows; i++) {
        if (sortKeys[i].index == i)
            continue;

        Row held = out->rows[i];
        int at   = i;

        for (;;) {
            int from = sortKeys[at].index;

            sortKeys[at].index = at;        /* this position is settled */

            if (from == i)
                break;

            out->rows[at] = out->rows[from];
            at = from;
        }

        out->rows[at] = held;
    }

    return SUCCESS_CODE;
}

/*
 * Result headers carry whatever qualification the schema gave them, so a join
 * heading a column "orders.total" still answers to plain "total" - as long as
 * only one header does, which is the same rule findColumn applies.
 */
static int findHeader(const ResultSet* out, const char* name, int* slot)
{
    *slot = -1;

    for (int c = ZERO; c < out->ncols; c++)
        if (_stricmp(out->headers[c], name) == ZERO) {
            *slot = c;
            return SUCCESS_CODE;
        }

    const char* wanted = strrchr(name, '.');
    wanted = wanted != NULL ? wanted + ONE : name;

    for (int c = ZERO; c < out->ncols; c++) {
        const char* header = strrchr(out->headers[c], '.');
        header = header != NULL ? header + ONE : out->headers[c];

        if (_stricmp(header, wanted) != ZERO)
            continue;
        if (*slot >= ZERO)
            return ERROR_SEMANTIC_AMBIGUOUS_COLUMN;

        *slot = c;
    }

    return *slot >= ZERO ? SUCCESS_CODE : ERROR_SEMANTIC_COLUMN_NOT_FOUND;
}

/*
 * HAVING names output columns, so every column in its expressions resolves
 * against the headers rather than against the table, the same way ORDER BY
 * terms do. The rows it will be run over are the ones being built, so a name
 * here is a label like "count(*)" rather than anything the catalog knows.
 */
static int resolveHavingExprs(Condition* cond, const ResultSet* out)
{
    for (int i = ZERO; i < cond->exprs.count; i++) {
        ExprNode* node = &cond->exprs.nodes[i];

        if (node->kind != EXPR_COLUMN)
            continue;

        int errorCode = findHeader(out, node->column, &node->slot);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }
    return SUCCESS_CODE;
}

static void freeOrder(void)
{
    free(sortKeys);
    sortKeys     = NULL;
    sortCapacity = ZERO;

    free(distinctBuckets);
    distinctBuckets = NULL;
    distinctCount   = ZERO;
}

static int rowsEqual(const Row* a, const Row* b, int ncols)
{
    for (int c = ZERO; c < ncols; c++)
        if (!valuesEqual(&a->values[c], &b->values[c]))
            return ZERO;

    return ONE;
}

/*
 * Drops duplicate result rows. NULLs count as equal to each other here, as
 * they do for GROUP BY - which is why this hashes with the same two functions
 * grouping uses rather than inventing a third notion of sameness.
 *
 * Kept rows go into an open-addressed table, so a row is checked against the
 * few that share its hash instead of against every row kept so far. The
 * pairwise version this replaces was quadratic: a distinct over 50,000 values
 * spent a billion comparisons finding that they were all different.
 */
static int applyDistinct(const SelectStatement* select, ResultSet* out)
{
    if (!select->distinct || out->nrows == ZERO)
        return SUCCESS_CODE;

    int wanted = ONE;
    while (wanted < out->nrows * TWO)       /* under half full, so probes stay short */
        wanted *= TWO;

    if (wanted > distinctCount) {
        int* moved = (int*)realloc(distinctBuckets, (size_t)wanted * sizeof(int));
        if (moved == NULL)
            return ERROR_EXEC_OUT_OF_MEMORY;

        distinctBuckets = moved;
        distinctCount   = wanted;
    }

    memset(distinctBuckets, ZERO, (size_t)distinctCount * sizeof(int));

    unsigned int mask = (unsigned int)distinctCount - ONE;
    int          kept = ZERO;

    for (int r = ZERO; r < out->nrows; r++) {
        const Row*   row  = &out->rows[r];
        unsigned int hash = 2166136261u;

        for (int c = ZERO; c < out->ncols; c++)
            hash = (hash ^ hashValue(&row->values[c])) * 16777619u;

        unsigned int probe = hash & mask;
        int          seen  = ZERO;

        for (; distinctBuckets[probe] != ZERO; probe = (probe + ONE) & mask) {
            if (rowsEqual(row, &out->rows[distinctBuckets[probe] - ONE],
                          out->ncols)) {
                seen = ONE;
                break;
            }
        }

        if (seen)
            continue;

        out->rows[kept++]     = *row;
        distinctBuckets[probe] = kept;      /* the row's new position, plus one */
    }

    out->nrows = kept;
    return SUCCESS_CODE;
}

static void applyLimit(const SelectStatement* select, ResultSet* out)
{
    if (select->limit >= ZERO && out->nrows > select->limit)
        out->nrows = select->limit;
}

/*
 * The materialised join. A Heap is about 2 MB, so this is static rather than a
 * local; one query joins at a time and nothing re-enters it.
 *
 * ponytail: nested loops, with WHERE applied to each combined row as it is
 * finished. The cap is therefore on rows kept rather than rows considered, so
 * two 500-row tables join fine as long as few pairs match - but the time is
 * still the full cross product. An index nested loop on the join column is the
 * upgrade when that starts to show.
 */
static THREAD_LOCAL Heap joinHeap;

static int joinTables(const SelectStatement* select, const Condition* where,
                      int t, int base, Row* work)
{
    if (t == select->ntables) {
        work->ncols   = base;
        work->deleted = ZERO;

        if (where->present
            && evaluateCondition(where, where->root, work) != TRI_TRUE)
            return SUCCESS_CODE;

        int landed;
        return heapInsert(&joinHeap, work, &landed);
    }

    const Heap* heap = findHeap(select->tables[t]);
    if (heap == NULL)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;

    const CatalogNode* node = findTable(select->tables[t]);
    if (node == NULL)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;

    /* For a LEFT JOIN this level decides which rows of its table may pair with
       the prefix above it, and whether any did. An inner join has neither: its
       ON was ANDed into the WHERE and is applied once, at the leaf. */
    const int onRoot  = select->onRoot[t];
    int       matched = ZERO;

    /*
     * Each level holds its page pinned while the levels under it run, so the
     * combined row can point straight at the source pages instead of copying
     * every text value it passes down. The pages are still pinned when the
     * innermost level writes the row into the join heap, which is the moment
     * the bytes are finally copied - once, into the record.
     */
    HeapScan             scan;
    int                  position;
    const unsigned char* record;
    Row                  source = { ZERO };

    heapScanStart(&scan, heap);

    while (heapScanNext(&scan, &position, &record)) {
        decodeRecord(record, &source, MAX_COLS - ONE);

        for (int c = ZERO; c < source.ncols; c++)
            work->values[base + c] = source.values[c];

        if (onRoot >= ZERO) {
            /* The ON sees the prefix and this row; the levels below are not
               filled yet and it cannot refer to them. */
            work->ncols = base + source.ncols;

            if (evaluateCondition(&select->where, onRoot, work) != TRI_TRUE)
                continue;

            matched = ONE;
        }

        int errorCode = joinTables(select, where, t + ONE,
                                   base + source.ncols, work);
        if (errorCode != SUCCESS_CODE) {
            heapScanEnd(&scan);
            return errorCode;
        }
    }

    heapScanEnd(&scan);

    /* Nothing on the right matched, and the join says the left row survives
       anyway - so it is completed with NULLs and passed down. `matched` is
       local to this call, which is one call per prefix, so it is asking
       exactly the right question. */
    if (select->outer[t] && !matched) {
        for (int c = ZERO; c < node->ncols; c++) {
            Value* value = &work->values[base + c];

            memset(value, ZERO, sizeof *value);
            value->isNull = ONE;
            value->type   = node->cols[c].type;
        }

        return joinTables(select, where, t + ONE, base + node->ncols, work);
    }

    return SUCCESS_CODE;
}

static int reserveJoinEntries(int rows)
{
    if (rows <= joinEntryCapacity)
        return SUCCESS_CODE;

    int grown = joinEntryCapacity ? joinEntryCapacity : INITIAL_RESULT_ROWS;
    while (grown < rows)
        grown *= TWO;

    JoinEntry* moved = (JoinEntry*)realloc(joinEntries,
                                           (size_t)grown * sizeof(JoinEntry));
    if (moved == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    joinEntries       = moved;
    joinEntryCapacity = grown;
    return SUCCESS_CODE;
}

/*
 * The one predicate that turns a join from a cross product into a lookup: an
 * equality whose two sides sit in different tables.
 *
 * Only the AND spine is walked, for the same reason index selection only walks
 * it - under an OR the other branch can match anything, so restricting the
 * pairs to those that share a key would drop rows that belong in the answer.
 */
static int findEquiJoin(const Condition* cond, int node,
                        int split, int* probeSlot, int* buildSlot)
{
    const ConditionNode* current = &cond->nodes[node];

    if (current->kind == COND_COMPARE) {
        const Predicate* compare = &current->compare;

        /* Only two plain columns can be keyed on. "a.id + 1 = b.uid" is an
           equality the nested loop will still check, but not one a hash table
           can be built from without computing the key for every row - which is
           a different optimisation from this one. */
        if (compare->op != OP_EQ
            || !exprIsColumn(&cond->exprs, compare->left)
            || !exprIsColumn(&cond->exprs, compare->right))
            return -1;

        int left  = cond->exprs.nodes[compare->left].slot;
        int right = cond->exprs.nodes[compare->right].slot;

        if (left < ZERO || right < ZERO)
            return -1;

        if (left < split && right >= split) {
            *probeSlot = left;
            *buildSlot = right - split;
            return node;
        }

        if (right < split && left >= split) {
            *probeSlot = right;
            *buildSlot = left - split;
            return node;
        }

        return -1;
    }

    if (current->kind != COND_AND)
        return -1;

    int found = findEquiJoin(cond, current->left, split, probeSlot, buildSlot);

    return found >= ZERO
         ? found
         : findEquiJoin(cond, current->right, split, probeSlot, buildSlot);
}

/*
 * Two tables joined on an equality, without considering every pair.
 *
 * The second table goes into a hash table keyed on its half of the equality;
 * the first is then scanned once, and each of its rows looks up only the rows
 * that could match it. That is O(n + m) where the nested loop is O(n * m) -
 * 15 million pairs became 50,300 lookups on the benchmark here.
 *
 * The rows it produces are the same rows, in the same order, whenever the build
 * side's key is unique - and the same set either way. Every pair it does form
 * is still put through the whole WHERE, so the equality it keyed on is checked
 * again along with everything else: the hash decides what to look at, never
 * what the answer is.
 *
 * NULL keys are dropped on both sides, because = against NULL is unknown and
 * an unknown row is not returned - which is what the nested loop's evaluation
 * concludes too, one pair at a time.
 */
static int hashJoin(const SelectStatement* select, const CatalogNode* schema,
                    Row* work, int split, int probeSlot, int buildSlot)
{
    const Heap* probeHeap = findHeap(select->tables[ZERO]);
    const Heap* buildHeap = findHeap(select->tables[ONE]);

    if (probeHeap == NULL || buildHeap == NULL)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;

    HeapScan             scan;
    HeapScan             fetch;
    int                  position;
    const unsigned char* record;
    Row                  row   = { ZERO };
    Row                  built = { ZERO };
    int                  count = ZERO;

    arenaRelease(&joinKeys);

    /* --- build: every row of the second table, under its key --- */
    heapScanStart(&scan, buildHeap);

    while (heapScanNext(&scan, &position, &record)) {
        decodeRecord(record, &row, buildSlot);

        const Value* key = &row.values[buildSlot];

        if (key->isNull)                    /* never equal to anything */
            continue;

        int errorCode = reserveJoinEntries(count + ONE);
        if (errorCode != SUCCESS_CODE) {
            heapScanEnd(&scan);
            return errorCode;
        }

        JoinEntry* entry = &joinEntries[count];

        entry->key      = *key;
        entry->hash     = hashValue(key);
        entry->position = position;

        /* the key outlives the page it was read from */
        if (key->type == TYPE_TEXT && key->textLength > ZERO) {
            entry->key.text = arenaCopy(&joinKeys, key->text, key->textLength);

            if (entry->key.text == NULL) {
                heapScanEnd(&scan);
                return ERROR_EXEC_OUT_OF_MEMORY;
            }
        }

        count++;
    }

    heapScanEnd(&scan);

    int wanted = 16;
    while (wanted < count * TWO)            /* under half full: short probes */
        wanted *= TWO;

    if (wanted > joinBucketCount) {
        int* moved = (int*)realloc(joinBuckets, (size_t)wanted * sizeof(int));
        if (moved == NULL)
            return ERROR_EXEC_OUT_OF_MEMORY;

        joinBuckets     = moved;
        joinBucketCount = wanted;
    }

    memset(joinBuckets, ZERO, (size_t)joinBucketCount * sizeof(int));

    unsigned int mask = (unsigned int)joinBucketCount - ONE;

    for (int i = ZERO; i < count; i++) {
        unsigned int probe = joinEntries[i].hash & mask;

        while (joinBuckets[probe] != ZERO)
            probe = (probe + ONE) & mask;

        joinBuckets[probe] = i + ONE;
    }

    /* --- probe: each row of the first table against its own key --- */
    heapScanStart(&scan, probeHeap);
    heapScanStart(&fetch, buildHeap);

    int errorCode = SUCCESS_CODE;

    while (errorCode == SUCCESS_CODE && heapScanNext(&scan, &position, &record)) {
        decodeRecord(record, &row, MAX_COLS - ONE);

        const Value* key = &row.values[probeSlot];

        if (key->isNull)
            continue;

        unsigned int hash = hashValue(key);

        /* Duplicates of one key sit in the run of slots after its own, so a
           walk that stops at the first empty slot sees all of them. */
        for (unsigned int at = hash & mask; joinBuckets[at] != ZERO;
             at = (at + ONE) & mask) {
            const JoinEntry* entry = &joinEntries[joinBuckets[at] - ONE];

            if (entry->hash != hash || !valuesEqual(&entry->key, key))
                continue;

            const unsigned char* other = heapScanAt(&fetch, entry->position);
            if (other == NULL)
                continue;

            decodeRecord(other, &built, MAX_COLS - ONE);

            for (int c = ZERO; c < row.ncols; c++)
                work->values[c] = row.values[c];
            for (int c = ZERO; c < built.ncols; c++)
                work->values[split + c] = built.values[c];

            work->ncols   = schema->ncols;
            work->deleted = ZERO;

            if (evaluateCondition(&select->where, select->where.root, work)
                != TRI_TRUE)
                continue;

            int landed;
            errorCode = heapInsert(&joinHeap, work, &landed);
            if (errorCode != SUCCESS_CODE)
                break;
        }
    }

    heapScanEnd(&fetch);
    heapScanEnd(&scan);

    if (errorCode == SUCCESS_CODE && explainEnabled)
        printf("[hash join: %d build row(s) -> %d row(s)]\n",
               count, heapLive(&joinHeap));

    return errorCode;
}

static int buildJoin(SelectStatement* select, const CatalogNode* schema)
{
    static THREAD_LOCAL Row work;       /* the recursion fills one row per thread */

    /* The schema is built after parsing, so this is the first time the WHERE
       has seen the columns it will actually be run against. */
    int errorCode = resolveCondition(schema, &select->where);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    heapReset(&joinHeap);
    snprintf(joinHeap.table, NAME_LEN, "%s", schema->table);

    /* The hash join drops left rows that find no partner, which is the whole
       difference between an inner join and an outer one. */
    int anyOuter = ZERO;
    for (int t = ZERO; t < select->ntables; t++)
        anyOuter |= select->outer[t];

    if (select->ntables == TWO && select->where.present && !anyOuter) {
        const CatalogNode* first = findTable(select->tables[ZERO]);
        int                probeSlot;
        int                buildSlot;

        if (first != NULL
            && findEquiJoin(&select->where, select->where.root,
                            first->ncols, &probeSlot, &buildSlot) >= ZERO)
            return hashJoin(select, schema, &work,
                            first->ncols, probeSlot, buildSlot);
    }

    errorCode = joinTables(select, &select->where, ZERO, ZERO, &work);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    if (explainEnabled)
        printf("[nested loop join: %d tables -> %d row(s)]\n",
               select->ntables, heapLive(&joinHeap));

    return SUCCESS_CODE;
}

static int executeSelect(SelectStatement* select, ResultSet* out)
{
    CatalogNode        joined;
    const CatalogNode* table;
    const Heap*        heap;

    /* A join - or a single aliased table, which needs the same requalification -
       is flattened into one schema and one heap of rows, so everything below
       this point is the single-table code, unchanged. */
    if (joinSchemaNeeded(select)) {
        /* through a const view: the statement is mutable here so that columns
           can be bound to the schema, but building it only reads */
        const SelectStatement* reading = select;

        int errorCode = buildJoinSchema(reading->tables, reading->aliases,
                                        reading->ntables, &joined);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        errorCode = buildJoin(select, &joined);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        table = &joined;
        heap  = &joinHeap;
    }
    else {
        table = findTable(select->table);
        heap  = findHeap(select->table);

        if (table == NULL || heap == NULL)
            return ERROR_SEMANTIC_TABLE_NOT_FOUND;
    }

    int errorCode = reserveCandidates(heapSlots(heap));
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    int* rows  = candidates;
    int  nrows = ZERO;

    int hasAggregate = ZERO;
    for (int i = ZERO; i < select->nitems; i++)
        if (select->items[i].aggregate != AGG_NONE)
            hasAggregate = ONE;

    /*
     * LIMIT can stop the scan itself, but only when nothing above it decides
     * which rows those are. ORDER BY, DISTINCT and grouping all do, so they
     * still need every matching row; a plain "first ten rows" does not, and
     * reads ten instead of the whole table.
     */
    int stopAt = -1;
    if (select->limit >= ZERO && select->norder == ZERO && !select->distinct
        && select->ngroup == ZERO && !hasAggregate)
        stopAt = select->limit;

    errorCode = collectRows(table, heap, &select->where, rows, &nrows, stopAt);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    if (select->ngroup == ZERO && !hasAggregate)
        errorCode = projectRows(select, table, heap, rows, nrows, out);
    else
        errorCode = projectGroups(select, table, heap, rows, nrows, out);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    errorCode = applyDistinct(select, out);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    errorCode = applyOrder(select, out);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    applyLimit(select, out);
    return SUCCESS_CODE;
}

/*
 * Empties a database and frees its slot. Every table is dropped through the
 * normal path, so heaps and index trees are released exactly as DROP TABLE
 * releases them - the list is re-read each time because dropping frees the
 * catalog nodes the previous listing pointed at.
 */
static int executeDropDatabase(const char* name, ResultSet* out)
{
    int id = findDatabase(name);
    if (id < ZERO)
        return ERROR_SEMANTIC_DATABASE_NOT_FOUND;
    if (id == ZERO)
        return ERROR_SEMANTIC_CANNOT_DROP_DEFAULT;

    int restoreTo = currentDatabaseId();
    selectDatabaseById(id);

    for (;;) {
        const CatalogNode* tables[MAX_TABLES];
        if (listTables(tables, MAX_TABLES) == ZERO)
            break;

        char tableName[NAME_LEN];
        snprintf(tableName, NAME_LEN, "%s", tables[ZERO]->table);

        dropIndexesForTable(tableName);
        dropHeap(tableName);
        dropTable(tableName);
    }

    /* dropping the database you are standing in leaves you in the default one */
    selectDatabaseById(restoreTo == id ? ZERO : restoreTo);
    releaseDatabase(id);

    snprintf(out->message, VALUE_LEN, "database %s dropped", name);
    return SUCCESS_CODE;
}

static int executeDropTable(const char* name, ResultSet* out)
{
    const CatalogNode* table = findTable(name);
    if (table == NULL)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;

    /* copy the stored spelling before the catalog node is freed */
    char resolved[NAME_LEN];
    snprintf(resolved, NAME_LEN, "%s", table->table);

    dropIndexesForTable(resolved);
    dropHeap(resolved);
    dropTable(resolved);

    snprintf(out->message, VALUE_LEN, "table %s dropped", resolved);
    return SUCCESS_CODE;
}

static int executeDropIndex(const char* name, ResultSet* out)
{
    int errorCode = dropIndexByName(name);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    snprintf(out->message, VALUE_LEN, "index %s dropped", name);
    return SUCCESS_CODE;
}

/*
 * Runs a checked statement. Sits where translateTokenLineToC sat in the compiler:
 * same tree walk, but it touches storage and fills a ResultSet instead of
 * appending C source to a buffer.
 */
/*
 * Runs every subquery the statement parsed, before the statement itself.
 *
 * Deepest first: parseSubquery takes its own slot before parsing what is
 * inside it, so a nested subquery always has the higher index, and walking the
 * pool backwards means a subquery's own subqueries have already produced their
 * values by the time it runs.
 *
 * Doing this before the outer query starts is what makes it safe at all. The
 * executor has one candidate array, one join heap and one group table, so a
 * subquery running in the middle of an outer scan would be walking over the
 * scan's own state. Nothing here overlaps: each subquery finishes completely,
 * and only its ResultSet outlives it.
 */
static int materialiseSubqueries(void)
{
    int total = subqueryTotal();

    for (int i = ZERO; i < MAX_SUBQUERIES; i++) {
        freeResultSet(&subqueryResult[i]);
        memset(&subqueryResult[i], ZERO, sizeof subqueryResult[i]);
    }

    for (int i = total - ONE; i >= ZERO; i--) {
        SelectStatement* sub = subqueryAt(i);

        if (sub == NULL)
            return ERROR_SEMANTIC_TABLE_NOT_FOUND;

        int errorCode = executeSelect(sub, &subqueryResult[i]);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        /* "x = (select ...)" wants one value. Nothing at all is NULL, which is
           legal; two rows is a question with no answer. */
        if (subqueryIsScalar(i) && subqueryResult[i].nrows > ONE)
            return ERROR_EXEC_SUBQUERY_NOT_SCALAR;
    }

    return SUCCESS_CODE;
}

int executeStatement(Statement* statement, ResultSet* out)
{
    if (subqueryTotal() > ZERO) {
        int errorCode = materialiseSubqueries();
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    switch (statement->type) {
    case STMT_CREATE_TABLE:
        return executeCreate(&statement->u.create, out);

    case STMT_CREATE_INDEX:
        return executeCreateIndex(&statement->u.createIndex, out);

    case STMT_INSERT:
        return executeInsert(&statement->u.insert, out);

    case STMT_SELECT:
        return executeSelect(&statement->u.select, out);

    case STMT_DELETE:
        return executeDelete(&statement->u.del, out);

    case STMT_UPDATE:
        return executeUpdate(&statement->u.update, out);

    case STMT_VACUUM:
        return executeVacuum(statement->u.vacuumTable, out);

    case STMT_ALTER_TABLE:
        return executeAlter(&statement->u.alter, out);

    case STMT_CREATE_DATABASE: {
        int errorCode = createDatabase(statement->u.databaseName);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        snprintf(out->message, VALUE_LEN, "database %s created",
                 statement->u.databaseName);
        return SUCCESS_CODE;
    }

    case STMT_USE_DATABASE: {
        int errorCode = useDatabase(statement->u.databaseName);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        snprintf(out->message, VALUE_LEN, "using database %s",
                 currentDatabaseName());
        return SUCCESS_CODE;
    }

    case STMT_DROP_DATABASE:
        return executeDropDatabase(statement->u.dropName, out);

    case STMT_DROP_TABLE:
        return executeDropTable(statement->u.dropName, out);

    case STMT_DROP_INDEX:
        return executeDropIndex(statement->u.dropName, out);

    case STMT_BEGIN: {
        int errorCode = beginTransaction();
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        snprintf(out->message, VALUE_LEN, "transaction started");
        return SUCCESS_CODE;
    }

    case STMT_COMMIT: {
        int errorCode = commitTransaction();
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        snprintf(out->message, VALUE_LEN, "transaction committed");
        return SUCCESS_CODE;
    }

    case STMT_ROLLBACK: {
        int errorCode = rollbackTransaction();
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        snprintf(out->message, VALUE_LEN, "transaction rolled back");
        return SUCCESS_CODE;
    }

    default:
        return GENERAL_ERROR;
    }
}
