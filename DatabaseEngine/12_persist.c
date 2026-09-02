#include "sql_common.h"

#ifdef _WIN32
/* Declared by hand rather than via <windows.h>: winnt.h defines a TokenType of
   its own, which collides with the lexer's. */
int __stdcall MoveFileExA(const char* from, const char* to, unsigned long flags);
#define MOVEFILE_REPLACE_EXISTING 0x1
#endif

/*
 * On-disk format, deliberately field by field rather than a raw struct dump:
 * struct padding and member order would otherwise become part of the file, and
 * adding a field would silently break every existing database.
 *
 * Version 8 is a page file with nothing after the pages:
 *
 *   header page (PAGE_SIZE bytes)
 *       magic "MINISQL1"      8 bytes
 *       version               u32
 *       page count            u32
 *       catalog root page     u32
 *   page 0 .. page count - 1  PAGE_SIZE each
 *
 * The catalog - database names, tables, their columns and what each one
 * promises, their page lists, index definitions -
 * lives in a chain of ordinary pages rather than in a section appended to the
 * file. That is what allows a page to be written back in place: with metadata
 * at the end, extending the page area would overwrite it. The chain also has
 * no size limit, which matters because a table's page list grows with the
 * table.
 *
 *   catalog page: u32 next (INVALID_CATALOG = end), u32 bytes used, payload
 *
 * Rows are not written one at a time - they are already in the pages - and
 * index nodes are pages too, so a tree is simply there when the file reopens.
 *
 * Earlier versions still load. Their metadata sits after the pages (3 and 4) or
 * is the whole file (1 and 2); either way it is read into one buffer and parsed
 * by the same code, then written back out as version 5.
 */

#define SAVE_MAGIC       "MINISQL1"
#define MAGIC_LENGTH     8
#define SAVE_VERSION     8u                     /* a CHECK holds expressions */
#define CHECKSUM_VERSION 7u                     /* pages carry checksums */
#define META_VERSION     6u                     /* column sizes, constraints */
#define CHAIN_VERSION    5u                     /* catalog inside the pages */
#define TAIL_VERSION     4u                     /* metadata after the pages */
#define TREELESS_VERSION 3u                     /* and indexes rebuilt on load */
#define ROWS_VERSION     2u                     /* rows written out one by one */
#define LEGACY_VERSION   1u                     /* single database, no names */

#define INVALID_CATALOG  0xFFFFFFFFu
#define CATALOG_HEADER   8
#define CATALOG_PAYLOAD  (PAGE_USABLE - CATALOG_HEADER)

/* ---------- little-endian bytes in memory ---------- */

typedef struct {
    unsigned char* data;
    size_t         used;
    size_t         capacity;
} Buffer;

typedef struct {
    const unsigned char* data;
    size_t               size;
    size_t               at;
} Reader;

static int bufReserve(Buffer* buffer, size_t extra)
{
    if (buffer->used + extra <= buffer->capacity)
        return SUCCESS_CODE;

    size_t grown = buffer->capacity ? buffer->capacity : 4096;
    while (grown < buffer->used + extra)
        grown *= TWO;

    unsigned char* moved = (unsigned char*)realloc(buffer->data, grown);
    if (moved == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    buffer->data     = moved;
    buffer->capacity = grown;
    return SUCCESS_CODE;
}

static int putU32(Buffer* buffer, unsigned int value)
{
    if (bufReserve(buffer, 4) != SUCCESS_CODE)
        return ERROR_EXEC_OUT_OF_MEMORY;

    unsigned char* at = buffer->data + buffer->used;

    at[ZERO] = (unsigned char)(value & 0xFFu);
    at[1]    = (unsigned char)((value >> 8) & 0xFFu);
    at[2]    = (unsigned char)((value >> 16) & 0xFFu);
    at[3]    = (unsigned char)((value >> 24) & 0xFFu);
    buffer->used += 4;
    return SUCCESS_CODE;
}

static int putBytes(Buffer* buffer, const void* bytes, size_t length)
{
    if (bufReserve(buffer, length) != SUCCESS_CODE)
        return ERROR_EXEC_OUT_OF_MEMORY;

    memcpy(buffer->data + buffer->used, bytes, length);
    buffer->used += length;
    return SUCCESS_CODE;
}

static int putString(Buffer* buffer, const char* text)
{
    size_t length = strlen(text);

    if (putU32(buffer, (unsigned int)length) != SUCCESS_CODE)
        return ERROR_EXEC_OUT_OF_MEMORY;

    return putBytes(buffer, text, length);
}

static int getU32(Reader* reader, unsigned int* value)
{
    if (reader->at + 4 > reader->size)
        return ERROR_IO_BAD_FORMAT;

    const unsigned char* at = reader->data + reader->at;

    *value = (unsigned int)at[ZERO]
           | ((unsigned int)at[1] << 8)
           | ((unsigned int)at[2] << 16)
           | ((unsigned int)at[3] << 24);
    reader->at += 4;
    return SUCCESS_CODE;
}

static int getBytes(Reader* reader, void* out, size_t length)
{
    if (reader->at + length > reader->size)
        return ERROR_IO_BAD_FORMAT;

    memcpy(out, reader->data + reader->at, length);
    reader->at += length;
    return SUCCESS_CODE;
}

static int getString(Reader* reader, char* out, unsigned int capacity)
{
    unsigned int length;

    if (getU32(reader, &length) != SUCCESS_CODE)
        return ERROR_IO_BAD_FORMAT;
    if (length >= capacity)                     /* refuse to overrun the buffer */
        return ERROR_IO_BAD_FORMAT;
    if (length > ZERO && getBytes(reader, out, length) != SUCCESS_CODE)
        return ERROR_IO_BAD_FORMAT;

    out[length] = '\0';
    return SUCCESS_CODE;
}

/* ---------- the catalog, serialised ---------- */

/*
 * A literal, as a DEFAULT or as the right side of a CHECK. Written in full
 * rather than by type, so reading one back never has to guess which of the
 * fields the writer thought was live.
 */
static int putValue(Buffer* out, const Value* value)
{
    unsigned char bits[8];
    int           errorCode = SUCCESS_CODE;

    memcpy(bits, &value->floatValue, 8);

    errorCode |= putU32(out, (unsigned int)value->type);
    errorCode |= putU32(out, (unsigned int)value->isNull);
    errorCode |= putU32(out, (unsigned int)value->intValue);
    errorCode |= putBytes(out, bits, 8);

    /* Only a text value has text. Asking any other kind for its bytes is
       asking about a field nothing filled in. */
    errorCode |= putString(out, value->type == TYPE_TEXT && !value->isNull
                                ? valueText(value) : "");

    return errorCode;
}

/*
 * A table's CHECK, node by node. The tree is a flat pool of at most
 * MAX_CONDITION_NODES entries with indices for edges, so it serialises as
 * itself - no traversal, and no pointers to rebuild.
 */
static int putCondition(Buffer* out, const Condition* cond)
{
    int present   = cond != NULL && cond->present;
    int errorCode = putU32(out, (unsigned int)present);

    if (!present)
        return errorCode;

    errorCode |= putU32(out, (unsigned int)cond->count);
    errorCode |= putU32(out, (unsigned int)cond->root);

    for (int i = ZERO; i < cond->count; i++) {
        const ConditionNode* node = &cond->nodes[i];

        errorCode |= putU32(out, (unsigned int)node->kind);
        errorCode |= putU32(out, (unsigned int)node->left);
        errorCode |= putU32(out, (unsigned int)node->right);

        /* Only a comparison has a predicate. An AND or a NOT never had one
           filled in, so writing its bytes would be writing whatever the pool
           happened to hold - and reading them back would be worse. */
        if (node->kind != COND_COMPARE)
            continue;

        errorCode |= putU32(out, (unsigned int)node->compare.left);
        errorCode |= putU32(out, (unsigned int)node->compare.op);
        errorCode |= putU32(out, (unsigned int)node->compare.right);
    }

    /* The operands themselves. A predicate is two indices into this, so the
       pool has to travel with the tree rather than beside it. */
    errorCode |= putU32(out, (unsigned int)cond->exprs.count);

    for (int i = ZERO; i < cond->exprs.count; i++) {
        const ExprNode* node = &cond->exprs.nodes[i];

        errorCode |= putU32(out, (unsigned int)node->kind);
        errorCode |= putU32(out, (unsigned int)node->op);
        errorCode |= putU32(out, (unsigned int)node->left);
        errorCode |= putU32(out, (unsigned int)node->right);
        errorCode |= putString(out, node->column);
        errorCode |= putValue(out, &node->literal);
    }

    return errorCode;
}

/*
 * Writes the database that is currently selected.
 */
static int saveCurrentDatabase(Buffer* out)
{
    const CatalogNode* tables[MAX_TABLES];
    int tableCount = listTables(tables, MAX_TABLES);
    int errorCode  = SUCCESS_CODE;

    errorCode |= putU32(out, (unsigned int)tableCount);

    for (int t = ZERO; t < tableCount; t++) {
        const CatalogNode* table = tables[t];

        errorCode |= putString(out, table->table);
        errorCode |= putU32(out, (unsigned int)table->ncols);

        for (int c = ZERO; c < table->ncols; c++) {
            const Column* column = &table->cols[c];

            errorCode |= putString(out, column->name);
            errorCode |= putU32(out, (unsigned int)column->type);
            errorCode |= putU32(out, (unsigned int)column->size);
            errorCode |= putU32(out, (unsigned int)column->flags);
            errorCode |= putU32(out, (unsigned int)column->hasDefault);

            if (column->hasDefault)
                errorCode |= putValue(out, &column->defaultValue);
        }

        errorCode |= putCondition(out, table->check);

        /* Which pages hold this table, and how many slots each one uses. That
           is all it takes to reconstruct the heap without touching a page. */
        const Heap* heap  = findHeap(table->table);
        int         pages = heap != NULL ? heapPageCount(heap) : ZERO;

        errorCode |= putU32(out, (unsigned int)pages);

        for (int i = ZERO; i < pages; i++) {
            errorCode |= putU32(out, (unsigned int)heapPageId(heap, i));
            errorCode |= putU32(out, (unsigned int)heapPageSlots(heap, i));
        }

        errorCode |= putU32(out, (unsigned int)(heap != NULL ? heapLive(heap) : ZERO));
    }

    const Index* indexes[MAX_INDEXES];
    int indexCount = listIndexes(indexes, MAX_INDEXES);

    errorCode |= putU32(out, (unsigned int)indexCount);

    for (int i = ZERO; i < indexCount; i++) {
        errorCode |= putString(out, indexes[i]->name);
        errorCode |= putString(out, indexes[i]->table);
        errorCode |= putString(out, indexes[i]->column);
        errorCode |= putU32(out, (unsigned int)indexRootPage(indexes[i]));
        errorCode |= putU32(out, (unsigned int)indexKeyCount(indexes[i]));
    }

    return errorCode != SUCCESS_CODE ? ERROR_EXEC_OUT_OF_MEMORY : SUCCESS_CODE;
}

static int saveCatalog(Buffer* out)
{
    int restoreTo = currentDatabaseId();

    if (putString(out, currentDatabaseName()) != SUCCESS_CODE
        || putU32(out, (unsigned int)databaseCount()) != SUCCESS_CODE)
        return ERROR_EXEC_OUT_OF_MEMORY;

    /* slots can have holes after a DROP, so walk slots and skip the free ones */
    for (int d = ZERO; d < databaseSlotCount(); d++) {
        if (!databaseInUse(d))
            continue;

        selectDatabaseById(d);

        int errorCode = putString(out, databaseName(d));
        if (errorCode == SUCCESS_CODE)
            errorCode = saveCurrentDatabase(out);

        if (errorCode != SUCCESS_CODE) {
            selectDatabaseById(restoreTo);
            return errorCode;
        }
    }

    selectDatabaseById(restoreTo);
    return SUCCESS_CODE;
}

/* ---------- the catalog, as a chain of pages ---------- */

static unsigned int pageU32(const Page* page, int offset)
{
    const unsigned char* at = page->data + offset;

    return (unsigned int)at[ZERO]
         | ((unsigned int)at[1] << 8)
         | ((unsigned int)at[2] << 16)
         | ((unsigned int)at[3] << 24);
}

static void setPageU32(Page* page, int offset, unsigned int value)
{
    unsigned char* at = page->data + offset;

    at[ZERO] = (unsigned char)(value & 0xFFu);
    at[1]    = (unsigned char)((value >> 8) & 0xFFu);
    at[2]    = (unsigned char)((value >> 16) & 0xFFu);
    at[3]    = (unsigned char)((value >> 24) & 0xFFu);
}

/*
 * Lays the serialised catalog across a chain of pages, reusing the chain that
 * is already there and extending or trimming it to fit. Returns the root.
 */
static int writeCatalogChain(const Buffer* blob, int* rootPage)
{
    size_t written = ZERO;
    int    previous = (int)INVALID_CATALOG;
    int    current  = *rootPage;
    int    first    = (int)INVALID_CATALOG;

    do {
        if (current == (int)INVALID_CATALOG) {
            int fresh;
            int errorCode = poolAllocate(&fresh);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            /* poolAllocate zeroes the page, so its next field would read as
               page 0 - a real page id - rather than end of chain. */
            Page* blank = poolPin(fresh);
            if (blank == NULL)
                return ERROR_EXEC_OUT_OF_MEMORY;

            setPageU32(blank, ZERO, INVALID_CATALOG);
            setPageU32(blank, 4, ZERO);
            poolUnpin(fresh, ONE);

            current = fresh;

            if (previous != (int)INVALID_CATALOG) {
                Page* back = poolPin(previous);
                if (back == NULL)
                    return ERROR_EXEC_OUT_OF_MEMORY;
                setPageU32(back, ZERO, (unsigned int)current);
                poolUnpin(previous, ONE);
            }
        }

        Page* page = poolPin(current);
        if (page == NULL)
            return ERROR_EXEC_OUT_OF_MEMORY;

        size_t chunk = blob->used - written;
        if (chunk > CATALOG_PAYLOAD)
            chunk = CATALOG_PAYLOAD;

        memcpy(page->data + CATALOG_HEADER, blob->data + written, chunk);
        setPageU32(page, 4, (unsigned int)chunk);
        written += chunk;

        int next = (int)pageU32(page, ZERO);

        if (written == blob->used) {            /* the chain ends here */
            setPageU32(page, ZERO, INVALID_CATALOG);
            poolUnpin(current, ONE);

            while (next != (int)INVALID_CATALOG) {   /* release what is left over */
                Page* stale = poolPin(next);
                if (stale == NULL)
                    break;

                int following = (int)pageU32(stale, ZERO);
                poolUnpin(next, ZERO);
                poolFree(next);
                next = following;
            }
        }
        else {
            poolUnpin(current, ONE);
        }

        if (first == (int)INVALID_CATALOG)
            first = current;

        previous = current;
        current  = next;
    } while (written < blob->used);

    *rootPage = first;
    return SUCCESS_CODE;
}

static int readCatalogChain(int rootPage, Buffer* out)
{
    int page = rootPage;

    while (page != (int)INVALID_CATALOG && page >= ZERO) {
        Page* pinned = poolPin(page);
        if (pinned == NULL)
            return ERROR_IO_BAD_FORMAT;

        unsigned int used = pageU32(pinned, 4);
        unsigned int next = pageU32(pinned, ZERO);

        /* Bounded by the page rather than by today's payload: a file written
           before the trailer existed put more in here, and it still has to be
           readable. */
        if (used > PAGE_SIZE - CATALOG_HEADER) {
            poolUnpin(page, ZERO);
            return ERROR_IO_BAD_FORMAT;
        }

        int errorCode = putBytes(out, pinned->data + CATALOG_HEADER, used);
        poolUnpin(page, ZERO);

        if (errorCode != SUCCESS_CODE)
            return errorCode;

        page = (int)next;
    }

    return SUCCESS_CODE;
}

/* ---------- reading a database back ---------- */

/*
 * Text read back here lands in the statement arena, which is where a value
 * parsed from a CREATE TABLE would have been too. addTable copies whatever it
 * keeps into the catalog's own arena, so both paths hand it the same thing.
 */
static int getValue(Reader* reader, Value* value)
{
    unsigned int  type;
    unsigned int  isNull;
    unsigned int  intValue;
    unsigned char bits[8];
    char          text[VALUE_LEN];

    if (getU32(reader, &type) != SUCCESS_CODE
        || type > (unsigned int)TYPE_DATE
        || getU32(reader, &isNull) != SUCCESS_CODE
        || getU32(reader, &intValue) != SUCCESS_CODE
        || getBytes(reader, bits, 8) != SUCCESS_CODE
        || getString(reader, text, VALUE_LEN) != SUCCESS_CODE)
        return ERROR_IO_BAD_FORMAT;

    if (isNull) {
        setNull(value, (ColType)type);
        return SUCCESS_CODE;
    }

    if (type == (unsigned int)TYPE_TEXT) {
        setText(value, text, (int)strlen(text));
        return SUCCESS_CODE;
    }

    value->type       = (ColType)type;
    value->isNull     = ZERO;
    value->intValue   = (int)intValue;
    value->text       = NULL;
    value->textLength = ZERO;
    memcpy(&value->floatValue, bits, 8);

    return SUCCESS_CODE;
}

static int getCondition(Reader* reader, Condition* out, int* present)
{
    unsigned int flag;

    if (getU32(reader, &flag) != SUCCESS_CODE)
        return ERROR_IO_BAD_FORMAT;

    *present     = (int)flag;
    out->present = (int)flag;
    out->count   = ZERO;
    out->root    = ZERO;
    exprInit(&out->exprs);

    if (!flag)
        return SUCCESS_CODE;

    unsigned int count;
    unsigned int root;

    if (getU32(reader, &count) != SUCCESS_CODE
        || count == ZERO || count > MAX_CONDITION_NODES
        || getU32(reader, &root) != SUCCESS_CODE || root >= count)
        return ERROR_IO_BAD_FORMAT;

    out->count = (int)count;
    out->root  = (int)root;

    for (unsigned int i = ZERO; i < count; i++) {
        ConditionNode* node = &out->nodes[i];

        unsigned int kind;
        unsigned int left;
        unsigned int right;
        unsigned int op;

        *node = (ConditionNode){ ZERO };

        if (getU32(reader, &kind) != SUCCESS_CODE || kind > (unsigned int)COND_NOT
            || getU32(reader, &left) != SUCCESS_CODE
            || getU32(reader, &right) != SUCCESS_CODE)
            return ERROR_IO_BAD_FORMAT;

        node->kind  = (ConditionKind)kind;
        node->left  = (int)left;                /* -1 arrives as 0xFFFFFFFF */
        node->right = (int)right;

        if (node->kind != COND_COMPARE)
            continue;

        unsigned int operandLeft;
        unsigned int operandRight;

        if (getU32(reader, &operandLeft) != SUCCESS_CODE
            || getU32(reader, &op) != SUCCESS_CODE || op > (unsigned int)OP_NOT_LIKE
            || getU32(reader, &operandRight) != SUCCESS_CODE)
            return ERROR_IO_BAD_FORMAT;

        node->compare.left  = (int)operandLeft;
        node->compare.op    = (CompareOp)op;
        node->compare.right = (int)operandRight;
        /* Not stored, because a CHECK cannot contain a subquery: the operator
           check above is what guarantees that, so -1 is the only value a
           loaded predicate can correctly have. */
        node->compare.subquery = -ONE;
    }

    unsigned int operands;

    if (getU32(reader, &operands) != SUCCESS_CODE || operands > MAX_EXPR_NODES)
        return ERROR_IO_BAD_FORMAT;

    exprInit(&out->exprs);
    out->exprs.count = (int)operands;

    for (unsigned int i = ZERO; i < operands; i++) {
        ExprNode* node = &out->exprs.nodes[i];

        unsigned int kind;
        unsigned int op;
        unsigned int left;
        unsigned int right;

        *node = (ExprNode){ ZERO };

        if (getU32(reader, &kind) != SUCCESS_CODE || kind > (unsigned int)EXPR_NEGATE
            || getU32(reader, &op) != SUCCESS_CODE || op > (unsigned int)ARITH_MOD
            || getU32(reader, &left) != SUCCESS_CODE
            || getU32(reader, &right) != SUCCESS_CODE
            || getString(reader, node->column, NAME_LEN) != SUCCESS_CODE
            || getValue(reader, &node->literal) != SUCCESS_CODE)
            return ERROR_IO_BAD_FORMAT;

        node->kind  = (ExprKind)kind;
        node->op    = (ArithOp)op;
        node->left  = (int)left;
        node->right = (int)right;
        node->slot  = -1;               /* addTable binds it to its table */
        node->type  = node->literal.type;
    }

    return SUCCESS_CODE;
}

static int readRow(Reader* reader, const CatalogNode* table, Row* row)
{
    *row = (Row){ ZERO };
    row->ncols = table->ncols;

    for (int c = ZERO; c < table->ncols; c++) {
        Value*        value = &row->values[c];
        unsigned char isNull;

        if (getBytes(reader, &isNull, ONE) != SUCCESS_CODE)
            return ERROR_IO_BAD_FORMAT;

        value->type = table->cols[c].type;

        if (isNull) {
            setNull(value, table->cols[c].type);
            continue;
        }

        value->isNull = ZERO;

        if (table->cols[c].type == TYPE_INT) {
            unsigned int stored;
            if (getU32(reader, &stored) != SUCCESS_CODE)
                return ERROR_IO_BAD_FORMAT;
            value->intValue = (int)stored;
        }
        else {
            char buffer[VALUE_LEN];

            if (getString(reader, buffer, VALUE_LEN) != SUCCESS_CODE)
                return ERROR_IO_BAD_FORMAT;

            setText(value, buffer, (int)strlen(buffer));
        }
    }

    return SUCCESS_CODE;
}

/*
 * Reads one database's tables and indexes into whichever database is currently
 * selected. Versions 4 and 5 restore each table's page list without reading a
 * page; older files carry the rows themselves and are replayed through
 * heapInsert, which quietly upgrades them on the next save.
 */
static int loadCurrentDatabase(Reader* reader, unsigned int version)
{
    int paged = (version >= TREELESS_VERSION);
    int trees = (version >= TAIL_VERSION);
    int meta  = (version >= META_VERSION);      /* column sizes and constraints */

    unsigned int tableCount;
    if (getU32(reader, &tableCount) != SUCCESS_CODE || tableCount > MAX_TABLES)
        return ERROR_IO_BAD_FORMAT;

    for (unsigned int t = ZERO; t < tableCount; t++) {
        char   tableName[NAME_LEN];
        Column cols[MAX_COLS];

        if (getString(reader, tableName, NAME_LEN) != SUCCESS_CODE)
            return ERROR_IO_BAD_FORMAT;

        unsigned int ncols;
        if (getU32(reader, &ncols) != SUCCESS_CODE || ncols == ZERO || ncols > MAX_COLS)
            return ERROR_IO_BAD_FORMAT;

        for (unsigned int c = ZERO; c < ncols; c++) {
            unsigned int type;

            cols[c] = (Column){ { 0 }, TYPE_INT, ZERO, ZERO, ZERO, { 0 } };

            if (getString(reader, cols[c].name, NAME_LEN) != SUCCESS_CODE
                || getU32(reader, &type) != SUCCESS_CODE
                || type > (unsigned int)TYPE_DATE)
                return ERROR_IO_BAD_FORMAT;

            cols[c].type = (ColType)type;

            if (!meta)                          /* older file: no promises kept */
                continue;

            unsigned int size;
            unsigned int flags;
            unsigned int hasDefault;

            if (getU32(reader, &size) != SUCCESS_CODE
                || getU32(reader, &flags) != SUCCESS_CODE
                || getU32(reader, &hasDefault) != SUCCESS_CODE)
                return ERROR_IO_BAD_FORMAT;

            cols[c].size       = (int)size;
            cols[c].flags      = (int)flags;
            cols[c].hasDefault = (int)hasDefault;

            if (hasDefault
                && getValue(reader, &cols[c].defaultValue) != SUCCESS_CODE)
                return ERROR_IO_BAD_FORMAT;
        }

        Condition check;
        int       hasCheck = ZERO;

        if (meta && getCondition(reader, &check, &hasCheck) != SUCCESS_CODE)
            return ERROR_IO_BAD_FORMAT;

        int errorCode = addTable(tableName, cols, (int)ncols,
                                 hasCheck ? &check : NULL);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        Heap* heap = createHeap(tableName);
        if (heap == NULL)
            return ERROR_EXEC_TOO_MANY_TABLES;

        const CatalogNode* table = findTable(tableName);

        if (paged) {
            unsigned int pages;
            if (getU32(reader, &pages) != SUCCESS_CODE)
                return ERROR_IO_BAD_FORMAT;

            for (unsigned int i = ZERO; i < pages; i++) {
                unsigned int pageId;
                unsigned int slots;

                if (getU32(reader, &pageId) != SUCCESS_CODE
                    || getU32(reader, &slots) != SUCCESS_CODE)
                    return ERROR_IO_BAD_FORMAT;

                errorCode = heapAdoptPage(heap, (int)pageId, (int)slots);
                if (errorCode != SUCCESS_CODE)
                    return errorCode;
            }

            unsigned int live;
            if (getU32(reader, &live) != SUCCESS_CODE)
                return ERROR_IO_BAD_FORMAT;

            heapSetLive(heap, (int)live);
            continue;
        }

        unsigned int rowCount;
        if (getU32(reader, &rowCount) != SUCCESS_CODE)
            return ERROR_IO_BAD_FORMAT;

        for (unsigned int r = ZERO; r < rowCount; r++) {
            Row row;

            errorCode = readRow(reader, table, &row);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            int landed;
            errorCode = heapInsert(heap, &row, &landed);
            if (errorCode != SUCCESS_CODE)
                return errorCode;
        }
    }

    unsigned int indexCount;
    if (getU32(reader, &indexCount) != SUCCESS_CODE)
        return ERROR_IO_BAD_FORMAT;

    for (unsigned int i = ZERO; i < indexCount; i++) {
        char name[NAME_LEN];
        char tableName[NAME_LEN];
        char column[NAME_LEN];

        if (getString(reader, name, NAME_LEN) != SUCCESS_CODE
            || getString(reader, tableName, NAME_LEN) != SUCCESS_CODE
            || getString(reader, column, NAME_LEN) != SUCCESS_CODE)
            return ERROR_IO_BAD_FORMAT;

        const CatalogNode* table = findTable(tableName);
        if (table == NULL)
            return ERROR_IO_BAD_FORMAT;

        int slot = findColumn(table, column);
        if (slot < ZERO)
            return ERROR_IO_BAD_FORMAT;

        int errorCode;

        if (trees) {
            unsigned int rootPage;
            unsigned int keyCount;

            if (getU32(reader, &rootPage) != SUCCESS_CODE
                || getU32(reader, &keyCount) != SUCCESS_CODE)
                return ERROR_IO_BAD_FORMAT;

            errorCode = adoptIndex(name, table->table, table->cols[slot].name,
                                   slot, (int)rootPage, (int)keyCount);
        }
        else {
            errorCode = createIndex(name, table->table, table->cols[slot].name,
                                    slot, table->cols[slot].type);
        }

        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    /* A tree that came back with the pages is ready; anything older left empty
       trees behind that have to be filled from the rows just loaded. */
    if (trees)
        return SUCCESS_CODE;

    const CatalogNode* tables[MAX_TABLES];
    int loaded = listTables(tables, MAX_TABLES);

    for (int t = ZERO; t < loaded; t++) {
        const Heap* heap = findHeap(tables[t]->table);
        if (heap == NULL)
            continue;

        int errorCode = rebuildIndexes(tables[t]->table, heap);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    return SUCCESS_CODE;
}

/* ---------- files ---------- */

/*
 * Renames over an existing file. Plain rename() refuses that on Windows;
 * MoveFileEx replaces atomically, as POSIX rename already does.
 */
static int replaceFile(const char* from, const char* to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != ZERO;
#else
    return rename(from, to) == ZERO;
#endif
}

/* The file the pool is faulting pages in from, so a save can tell whether it is
   about to replace the ground it is standing on. */
static char openPath[LINE_LEN];
static int  catalogRoot = (int)INVALID_CATALOG;

/*
 * Whether a file is one this build may write single pages into: laid out in
 * pages, and of the current version. An older layout keeps its metadata
 * somewhere a page write would land on top of.
 */
int isPageFile(const char* path)
{
    unsigned char head[12];
    FILE*         file = fopen(path, "rb");

    if (file == NULL)
        return ZERO;

    int current = fread(head, ONE, sizeof head, file) == sizeof head
                  && memcmp(head, SAVE_MAGIC, MAGIC_LENGTH) == ZERO
                  && head[8] == (unsigned char)SAVE_VERSION
                  && head[9] == ZERO && head[10] == ZERO && head[11] == ZERO;

    fclose(file);
    return current;
}

const char* databasePath(void)
{
    return openPath[ZERO] != '\0' ? openPath : NULL;
}

/*
 * Hands the pool the file again after a save has replaced it. Failing here is
 * survivable: the pages are still in memory, they just cannot be re-read once
 * evicted, so the pool stops evicting rather than losing anything.
 */
static void reopenPageFile(const char* path, int writable)
{
    FILE* reopened = fopen(path, writable ? "r+b" : "rb");

    if (reopened == NULL)
        return;

    /* A pool that was writing in place has to keep being able to, or its dirty
       pages have nowhere to go and the log it is still appending to describes
       a file nothing ever writes. */
    if (writable)
        poolSetWritable(reopened, poolPageCount());
    else
        poolAdopt(reopened, poolPageCount());
}

static void writeHeader(unsigned char* header, int pages, int catalog)
{
    memset(header, ZERO, PAGE_SIZE);
    memcpy(header, SAVE_MAGIC, MAGIC_LENGTH);

    header[8] = (unsigned char)(SAVE_VERSION & 0xFFu);

    unsigned int values[TWO] = { (unsigned int)pages, (unsigned int)catalog };

    for (int v = ZERO; v < TWO; v++)
        for (int b = ZERO; b < 4; b++)
            header[12 + v * 4 + b] = (unsigned char)((values[v] >> (b * 8)) & 0xFFu);
}

/*
 * Writes the catalog into its pages, then streams every page into a new file
 * and renames it into place.
 */
int saveDatabase(const char* path)
{
    Buffer blob = { NULL, ZERO, ZERO };

    int errorCode = saveCatalog(&blob);
    if (errorCode != SUCCESS_CODE) {
        free(blob.data);
        return errorCode;
    }

    errorCode = writeCatalogChain(&blob, &catalogRoot);
    free(blob.data);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    char temp[LINE_LEN];
    if (snprintf(temp, sizeof temp, "%s.tmp", path) >= (int)sizeof temp)
        return ERROR_IO_CANNOT_OPEN;

    FILE* file = fopen(temp, "wb");
    if (file == NULL)
        return ERROR_IO_CANNOT_OPEN;

    unsigned char header[PAGE_SIZE];
    writeHeader(header, poolPageCount(), catalogRoot);

    if (fwrite(header, ONE, PAGE_SIZE, file) != PAGE_SIZE) {
        fclose(file);
        remove(temp);
        return ERROR_IO_WRITE;
    }

    int pageError = poolWriteAll(file);
    if (pageError != SUCCESS_CODE || ferror(file)) {
        fclose(file);
        remove(temp);
        return pageError != SUCCESS_CODE ? pageError : ERROR_IO_WRITE;
    }

    /* The rename is atomic, but only over a file whose contents have actually
       reached the disk. */
    if (walSyncHandle(file) != SUCCESS_CODE) {
        fclose(file);
        remove(temp);
        return ERROR_IO_WRITE;
    }

    if (fclose(file) != ZERO) {
        remove(temp);
        return ERROR_IO_WRITE;
    }

    /* Windows will not rename over a file that is still open, and every page
       has just been written to the replacement anyway. */
    int replacingOpenFile = openPath[ZERO] != '\0' && _stricmp(openPath, path) == ZERO;
    int wasWritable       = poolIsWritable();

    if (replacingOpenFile)
        poolDetachFile();

    if (!replaceFile(temp, path)) {
        remove(temp);
        if (replacingOpenFile)
            reopenPageFile(path, wasWritable);  /* put back what we let go of */
        return ERROR_IO_WRITE;
    }

    /* The pages are in *this* file, which is only the file the pool is reading
       from when we have just replaced it. Marking them clean after a save to
       some other path would let them be evicted and dropped, and the only copy
       would be in a file this session is not using. */
    if (replacingOpenFile || !poolIsWritable())
        poolMarkAllClean();

    if (replacingOpenFile) {
        reopenPageFile(path, wasWritable);

        /* The file being paged from is now one this build wrote, so every page
           in it carries a checksum and every read may be checked. */
        poolSetChecksums(ONE);
    }

    /* A whole file has just been written, so any log for it is describing a
       database that no longer exists. */
    walDiscard(path);

    return SUCCESS_CODE;
}

/*
 * Reads the metadata region of a pre-version-5 file, which sits after the pages
 * rather than inside them, into one buffer.
 */
static int readTail(FILE* file, long from, Buffer* out)
{
    if (fseek(file, ZERO, SEEK_END) != ZERO)
        return ERROR_IO_BAD_FORMAT;

    long end = ftell(file);
    if (end < from)
        return ERROR_IO_BAD_FORMAT;

    size_t length = (size_t)(end - from);

    if (bufReserve(out, length) != SUCCESS_CODE)
        return ERROR_EXEC_OUT_OF_MEMORY;
    if (fseek(file, from, SEEK_SET) != ZERO)
        return ERROR_IO_BAD_FORMAT;
    if (length > ZERO && fread(out->data, ONE, length, file) != length)
        return ERROR_IO_BAD_FORMAT;

    out->used = length;
    return SUCCESS_CODE;
}

/* ---------- transactions ---------- */

/*
 * Without BEGIN every statement is its own transaction and pays one fsync.
 * BEGIN suspends that until COMMIT, which is what makes a bulk load bearable.
 *
 * BEGIN checkpoints first, and that is the whole trick behind ROLLBACK: after a
 * checkpoint the file holds everything committed, so any dirty page from then
 * on belongs to this transaction and nothing else. Rolling back is therefore
 * "drop the dirty pages and read the catalog again", with no undo log and no
 * before-images - the file is the before-image.
 */
static int transactionOpen;
static int transactionPages;                    /* the file's size at BEGIN */

int inTransaction(void)
{
    return transactionOpen;
}

int beginTransaction(void)
{
    if (transactionOpen)
        return ERROR_EXEC_TRANSACTION_ACTIVE;

    int errorCode = poolCheckpoint();
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    transactionPages = poolPageCount();
    transactionOpen  = ONE;
    return SUCCESS_CODE;
}

/*
 * Ends the transaction and leaves the durability to the caller: ProcessStatement
 * runs commitDatabase after every statement that is not inside one, and this
 * statement has just stopped being inside one.
 */
int commitTransaction(void)
{
    if (!transactionOpen)
        return ERROR_EXEC_NO_TRANSACTION;

    transactionOpen = ZERO;
    return SUCCESS_CODE;
}

/*
 * Throws the transaction's pages away and rebuilds the session from the file.
 * The reload is not laziness for its own sake: heaps, index roots and the
 * catalog live in memory as well as in pages, and reading them back is the only
 * thing that returns all three to the state the file describes.
 */
int rollbackTransaction(void)
{
    if (!transactionOpen)
        return ERROR_EXEC_NO_TRANSACTION;

    if (openPath[ZERO] == '\0')                 /* :memory:, or a legacy file */
        return ERROR_EXEC_CANNOT_ROLLBACK;

    char path[LINE_LEN];
    int  hadLog = walIsOpen();

    snprintf(path, sizeof path, "%s", openPath);

    transactionOpen = ZERO;
    poolRollback(transactionPages);

    int errorCode = loadDatabase(path);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /* Only put back what was there. A file too old to be written in place had
       no log to begin with, and giving it one here would start doing exactly
       what opening it decided not to do. */
    return hadLog ? openForWrite(path) : SUCCESS_CODE;
}

/*
 * Puts the catalog into its pages and commits everything the statement touched.
 *
 * The catalog has to go first. It records which pages belong to which table, so
 * a committed INSERT whose new page the catalog does not mention would come
 * back after a crash as a page belonging to nothing.
 */
int commitDatabase(void)
{
    if (!walIsOpen() || !poolHasDirty())
        return SUCCESS_CODE;

    Buffer blob = { NULL, ZERO, ZERO };

    int errorCode = saveCatalog(&blob);
    if (errorCode == SUCCESS_CODE)
        errorCode = writeCatalogChain(&blob, &catalogRoot);


    free(blob.data);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    poolSetCatalogRoot(catalogRoot);
    return poolCommit();
}

/*
 * Reopens the page file for writing and attaches the log, which together turn
 * the pool from a read cache into something that can push pages back out.
 */
int openForWrite(const char* path)
{
    FILE* file = fopen(path, "r+b");
    if (file == NULL)
        return ERROR_IO_CANNOT_OPEN;

    /* Only a current-version file is ever opened this way - main asks
       isPageFile first - so its pages carry checksums, including the ones a
       freshly created database has just been written with. */
    poolSetWritable(file, poolPageCount());
    poolSetChecksums(ONE);
    poolSetCatalogRoot(catalogRoot);
    snprintf(openPath, sizeof openPath, "%s", path);

    return walOpen(path);
}

/*
 * Folds the log into the database file and removes it. After this the file
 * stands on its own, which is what makes a clean exit leave one file behind
 * rather than two.
 */
int closeDatabase(void)
{
    int errorCode = poolCheckpoint();

    /* A checkpoint that failed leaves the log as the only complete record of
       what was committed. Removing it then would be the one way to lose data
       that the log exists to prevent. */
    if (errorCode == SUCCESS_CODE)
        walClose();

    return errorCode;
}

/*
 * Moves every row off the bytes the pool now stamps a checksum into.
 *
 * A page written before there was a trailer put records in those last bytes,
 * so writing that page back would destroy whatever lives there. Rewriting each
 * heap onto fresh pages moves them out of the way - which is what VACUUM
 * already does, so it is the same call - and the indexes follow, because
 * compaction renumbers every row.
 *
 * One pass, once, when an older database is opened.
 */
static int reservePageTrailers(void)
{
    int restoreTo = currentDatabaseId();

    for (int d = ZERO; d < databaseSlotCount(); d++) {
        if (!databaseInUse(d))
            continue;

        selectDatabaseById(d);

        const CatalogNode* tables[MAX_TABLES];
        int count = listTables(tables, MAX_TABLES);

        for (int t = ZERO; t < count; t++) {
            Heap* heap = findHeap(tables[t]->table);

            if (heap == NULL)
                continue;

            int reclaimed;
            int errorCode = heapCompact(heap, &reclaimed);

            if (errorCode == SUCCESS_CODE)
                errorCode = rebuildIndexes(tables[t]->table, heap);

            if (errorCode != SUCCESS_CODE) {
                selectDatabaseById(restoreTo);
                return errorCode;
            }
        }
    }

    selectDatabaseById(restoreTo);
    return SUCCESS_CODE;
}

int loadDatabase(const char* path)
{
    /* Whatever is open now is about to be dropped, so fold its log back into
       its own file first. Otherwise the pool would carry a log belonging to
       one database while faulting pages from another. */
    if (walIsOpen())
        closeDatabase();

    FILE* file = fopen(path, "rb");
    if (file == NULL)
        return ERROR_IO_CANNOT_OPEN;

    unsigned char head[24];
    if (fread(head, ONE, sizeof head, file) != sizeof head
        || memcmp(head, SAVE_MAGIC, MAGIC_LENGTH) != ZERO) {
        fclose(file);
        return ERROR_IO_BAD_FORMAT;
    }

    Reader header = { head, sizeof head, MAGIC_LENGTH };

    unsigned int version;
    if (getU32(&header, &version) != SUCCESS_CODE) {
        fclose(file);
        return ERROR_IO_BAD_FORMAT;
    }

    if (version > SAVE_VERSION || version < LEGACY_VERSION) {
        fclose(file);
        return ERROR_IO_VERSION;
    }

    int paged = (version >= TREELESS_VERSION);

    unsigned int pageTotal = ZERO;
    unsigned int catalog   = INVALID_CATALOG;

    if (paged && getU32(&header, &pageTotal) != SUCCESS_CODE) {
        fclose(file);
        return ERROR_IO_BAD_FORMAT;
    }
    if (version >= CHAIN_VERSION && getU32(&header, &catalog) != SUCCESS_CODE) {
        fclose(file);
        return ERROR_IO_BAD_FORMAT;
    }

    freeIndexes();
    freeCatalog();
    freeStorage();
    poolClear();
    initDatabases();
    initCatalog();
    initStorage();
    initIndexes();
    poolInit();

    /* The pool gets its own handle. Sharing this one would be a trap: reading a
       page seeks, and older formats are still being read sequentially from it. */
    if (paged) {
        FILE* pages = fopen(path, "rb");

        if (pages == NULL) {
            fclose(file);
            return ERROR_IO_CANNOT_OPEN;
        }

        poolAdopt(pages, (int)pageTotal);

        /* Before the first page is touched: the catalog chain is read below,
           and a corrupt catalog page is exactly the one worth catching. */
        poolSetChecksums(version >= CHECKSUM_VERSION);
    }

    Buffer blob = { NULL, ZERO, ZERO };
    int    errorCode;

    if (version >= CHAIN_VERSION) {
        errorCode = readCatalogChain((int)catalog, &blob);
    }
    else {
        long from = version == LEGACY_VERSION ? 12
                  : version == ROWS_VERSION   ? 12
                  : (long)PAGE_SIZE * ((long)pageTotal + ONE);

        errorCode = readTail(file, from, &blob);
    }

    fclose(file);

    if (errorCode != SUCCESS_CODE) {
        free(blob.data);
        return errorCode;
    }

    Reader reader = { blob.data, blob.used, ZERO };

    char         currentName[NAME_LEN];
    unsigned int total = ONE;

    /* version 1 predates databases: the whole file is one unnamed database */
    if (version != LEGACY_VERSION) {
        if (getString(&reader, currentName, NAME_LEN) != SUCCESS_CODE
            || getU32(&reader, &total) != SUCCESS_CODE || total > MAX_DATABASES) {
            free(blob.data);
            return ERROR_IO_BAD_FORMAT;
        }
    }
    else {
        snprintf(currentName, NAME_LEN, "%s", DEFAULT_DB_NAME);
    }

    errorCode = SUCCESS_CODE;

    if (version == LEGACY_VERSION) {
        selectDatabaseById(ZERO);                   /* everything lands in "main" */
        errorCode = loadCurrentDatabase(&reader, version);
    }
    else {
        for (unsigned int d = ZERO; d < total && errorCode == SUCCESS_CODE; d++) {
            char name[NAME_LEN];

            if (getString(&reader, name, NAME_LEN) != SUCCESS_CODE) {
                errorCode = ERROR_IO_BAD_FORMAT;
                break;
            }

            int id = findDatabase(name);            /* "main" already exists */
            if (id < ZERO) {
                errorCode = createDatabase(name);
                if (errorCode != SUCCESS_CODE)
                    break;
                id = findDatabase(name);
            }

            selectDatabaseById(id);
            errorCode = loadCurrentDatabase(&reader, version);
        }
    }

    free(blob.data);

    if (paged)
        snprintf(openPath, sizeof openPath, "%s", path);
    else
        openPath[ZERO] = '\0';

    catalogRoot = version >= CHAIN_VERSION ? (int)catalog : (int)INVALID_CATALOG;
    poolSetCatalogRoot(catalogRoot);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /*
     * An older page's last bytes are records rather than a checksum, which is
     * why nothing above turned the check on for one. Move the rows clear of
     * the trailer, so that everything written from here on can be stamped.
     */
    if (version < CHECKSUM_VERSION) {
        errorCode = reservePageTrailers();

        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    /* a missing name would mean a corrupt file, so fall back to the default */
    if (useDatabase(currentName) != SUCCESS_CODE)
        selectDatabaseById(ZERO);

    return SUCCESS_CODE;
}
