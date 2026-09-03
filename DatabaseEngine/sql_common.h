/*
 * Two POSIX things this engine uses are hidden by a strict -std=c17: strcasecmp
 * lives in <strings.h> behind a feature test, and PTHREAD_MUTEX_RECURSIVE is
 * XSI rather than base POSIX. Compiling with -std=c17 defines __STRICT_ANSI__,
 * which switches glibc's defaults off - so the build command in the README
 * would fail on Linux without this, while MinGW never noticed because it is
 * not glibc.
 *
 * Apple's headers go the other way and *remove* declarations under a bare
 * _XOPEN_SOURCE, so they get the macro they actually want.
 */
#ifndef _WIN32
#  ifdef __APPLE__
#    define _DARWIN_C_SOURCE 1
#  else
#    define _XOPEN_SOURCE 700
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#ifndef SQL_COMMON_H
#define SQL_COMMON_H

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Case-insensitive compare is spelled differently on each side. The engine
   says _stricmp throughout because that is what MSVC and MinGW provide; on
   anything else it is strcasecmp, which is POSIX rather than C. Defined here
   rather than per-file because every stage compares identifiers. */
#ifndef _WIN32
#include <strings.h>
#define _stricmp  strcasecmp
#define _strnicmp strncasecmp
#endif

/*
 * Per-thread state.
 *
 * A connection gets a thread, and everything a *statement* works in is private
 * to it: the text arena a scan winds back, the executor's candidate array,
 * group table, join heap and sort buffers, the subquery pool, and which
 * database USE left this connection on. Marking them thread-local rather than
 * threading a context through every function is what keeps this a small change
 * to a large executor - each one is already written as scratch space for one
 * statement, and that is exactly what a thread now has its own of.
 *
 * What is deliberately *not* thread-local is the data: the catalog, the heaps,
 * the indexes, the buffer pool and the log are one copy, shared, and reached
 * only under the engine lock.
 */
#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__)
#define THREAD_LOCAL __thread
#else
#define THREAD_LOCAL _Thread_local
#endif

/* "YYYY-MM-DD" and its terminator. */
#define DATE_TEXT_LEN   11

#define MAX_TOKENS      100
#define MAX_COLS        16
/* Rows are no longer capped: heaps page and result sets grow. This is only
   the starting size of a result set, which doubles from here. */
#define INITIAL_RESULT_ROWS 256
#define MAX_TABLES      64
#define MAX_INDEXES     64
#define MAX_DATABASES   8
#define MAX_JOIN_TABLES 4               /* tables allowed in one FROM list */
/* A SelectStatement is ~18 KB, so these live in one static pool rather than
   inside the statement that mentions them - which would make the type
   recursive and the statement enormous. */
#define MAX_SUBQUERIES  4
/* the synthetic table a join is projected through. Angle brackets are not
   valid in an identifier, so no real table can collide with it - which also
   means findIndexOn never matches it and a join always scans. */
#define JOIN_SCHEMA_NAME "<join>"
#define DEFAULT_DB_NAME "main"
#define MAX_GROUPS      128
#define MAX_CONDITION_NODES 32
#define MAX_EXPR_NODES  32              /* per expression pool */
#define BTREE_ORDER     8               /* max keys per B-tree node */
#define NAME_LEN        64
#define LINE_LEN        8192
/* Only a scratch buffer size now, not a limit on stored text: values and tokens
   point into an arena, so the real ceiling is what fits on one input line. */
#define VALUE_LEN       LINE_LEN
#define CATALOG_SIZE    101

#define SUCCESS_CODE 0

/* findColumn: -1 is no such column, this is "more than one table has it" */
#define COLUMN_AMBIGUOUS (-2)

#define ERROR_UNKNOWN_TOKEN                 100
#define ERROR_UNTERMINATED_STRING           101
#define ERROR_TOO_MANY_TOKENS               102
#define ERROR_VALUE_OUT_OF_RANGE            103
#define ERROR_TOKEN_TOO_LONG                104

#define ERROR_SYNTAX_INVALID_STATEMENT      200
#define ERROR_SYNTAX_EXPECTED_FROM          201
#define ERROR_SYNTAX_EXPECTED_TABLE_NAME    202
#define ERROR_SYNTAX_EXPECTED_COLUMN        203
#define ERROR_SYNTAX_EXPECTED_TYPE          204
#define ERROR_SYNTAX_EXPECTED_PARENTHESES   205
#define ERROR_SYNTAX_TRAILING_TOKENS        206
#define ERROR_SYNTAX_TOO_MANY_COLUMNS       207
#define ERROR_SYNTAX_EXPECTED_VALUES        208
#define ERROR_SYNTAX_EXPECTED_VALUE         209
#define ERROR_SYNTAX_EXPECTED_OPERATOR      210
#define ERROR_SYNTAX_EXPECTED_BY            211
#define ERROR_SYNTAX_UNKNOWN_FUNCTION       212
#define ERROR_SYNTAX_EXPECTED_NULL          213
#define ERROR_SYNTAX_EXPECTED_ON            214
#define ERROR_SYNTAX_EXPECTED_SET           215
#define ERROR_SYNTAX_EXPECTED_ASSIGNMENT    216
#define ERROR_SYNTAX_CONDITION_TOO_COMPLEX  217
#define ERROR_SYNTAX_TOO_MANY_TABLES        218
#define ERROR_SYNTAX_EXPECTED_KEY           219
#define ERROR_SYNTAX_EXPECTED_SIZE          220
#define ERROR_SYNTAX_EXPRESSION_TOO_COMPLEX 221
#define ERROR_SYNTAX_EXPECTED_TABLE         222
#define ERROR_SYNTAX_EXPECTED_TO            223
#define ERROR_SYNTAX_TOO_MANY_SUBQUERIES    224

#define ERROR_SEMANTIC_TABLE_NOT_FOUND      300
#define ERROR_SEMANTIC_TABLE_EXISTS         301
#define ERROR_SEMANTIC_COLUMN_NOT_FOUND     302
#define ERROR_SEMANTIC_TYPE_MISMATCH        303
#define ERROR_SEMANTIC_COLUMN_COUNT         304
#define ERROR_SEMANTIC_DUPLICATE_COLUMN     305
#define ERROR_SEMANTIC_NOT_GROUPED          306
#define ERROR_SEMANTIC_INDEX_EXISTS         307
#define ERROR_SEMANTIC_INDEX_NOT_FOUND      308
#define ERROR_SEMANTIC_HAVING_WITHOUT_GROUP 309
#define ERROR_SEMANTIC_DATABASE_EXISTS      310
#define ERROR_SEMANTIC_DATABASE_NOT_FOUND   311
#define ERROR_SEMANTIC_CANNOT_DROP_DEFAULT  312
#define ERROR_SEMANTIC_AMBIGUOUS_COLUMN     313
#define ERROR_SEMANTIC_DUPLICATE_TABLE      314
#define ERROR_SEMANTIC_JOIN_TOO_WIDE        315
#define ERROR_SEMANTIC_MISSING_VALUE        316
#define ERROR_SEMANTIC_INVALID_DATE         317
#define ERROR_SEMANTIC_LAST_COLUMN          318
#define ERROR_SEMANTIC_ALTER_UNSUPPORTED    319
#define ERROR_SEMANTIC_CHECK_BLOCKS_DROP    320
#define ERROR_SEMANTIC_CORRELATED_SUBQUERY   321
#define ERROR_SEMANTIC_SUBQUERY_COLUMNS      322
#define ERROR_EXEC_TOO_MANY_DATABASES       404

#define ERROR_EXEC_TABLE_FULL               400
#define ERROR_EXEC_TOO_MANY_TABLES          401
#define ERROR_EXEC_TOO_MANY_GROUPS          402
#define ERROR_EXEC_TOO_MANY_INDEXES         403
#define ERROR_EXEC_JOIN_TOO_LARGE           405
#define ERROR_EXEC_ROW_TOO_LARGE            406
#define ERROR_EXEC_OUT_OF_MEMORY            407
#define ERROR_EXEC_TRANSACTION_ACTIVE       408
#define ERROR_EXEC_NO_TRANSACTION           409
#define ERROR_EXEC_CANNOT_ROLLBACK          410
#define ERROR_EXEC_NOT_NULL                 411
#define ERROR_EXEC_NOT_UNIQUE               412
#define ERROR_EXEC_CHECK_FAILED             413
#define ERROR_EXEC_VALUE_TOO_LONG           414
#define ERROR_EXEC_DIVIDE_BY_ZERO           415
#define ERROR_EXEC_TABLE_TOO_WIDE           416
#define ERROR_EXEC_SUBQUERY_NOT_SCALAR      417

#define ERROR_IO_CANNOT_OPEN                600
#define ERROR_IO_BAD_FORMAT                 601
#define ERROR_IO_VERSION                    602
#define ERROR_IO_WRITE                      603
#define ERROR_IO_CHECKSUM                   604
#define ERROR_IO_TIMED_OUT                  605

#define GENERAL_ERROR                       501

/*
 * Packs four characters into the integer that a fixed-size memcpy of those same
 * four bytes loads, which turns a chain of string compares into one switch the
 * compiler can lay out as a jump table.
 *
 * The byte order is the host's, so the constant here and the memcpy agree only
 * on a little-endian machine. That is the same assumption the rest of the
 * Windows-only code already makes - and unlike the file format, which is
 * explicitly little-endian because it outlives the process, nothing packed this
 * way is ever written down.
 */
#define PACK_4(c0, c1, c2, c3)                 \
    ((uint32_t)(uint8_t)(c0)                   \
     | ((uint32_t)(uint8_t)(c1) << 8)          \
     | ((uint32_t)(uint8_t)(c2) << 16)         \
     | ((uint32_t)(uint8_t)(c3) << 24))

#define ZERO 0
#define ONE  1
#define TWO  2

/* ---------- lexer ---------- */

typedef enum {
    TOKEN_KEYWORD_SELECT,
    TOKEN_KEYWORD_FROM,
    TOKEN_KEYWORD_CREATE,
    TOKEN_KEYWORD_TABLE,
    TOKEN_KEYWORD_INSERT,
    TOKEN_KEYWORD_INTO,
    TOKEN_KEYWORD_VALUES,
    TOKEN_KEYWORD_INT,
    TOKEN_KEYWORD_TEXT,
    TOKEN_KEYWORD_WHERE,
    TOKEN_KEYWORD_GROUP,
    TOKEN_KEYWORD_BY,
    TOKEN_KEYWORD_NULL,
    TOKEN_KEYWORD_IS,
    TOKEN_KEYWORD_NOT,
    TOKEN_KEYWORD_INDEX,
    TOKEN_KEYWORD_ON,
    TOKEN_KEYWORD_DELETE,
    TOKEN_KEYWORD_UPDATE,
    TOKEN_KEYWORD_SET,
    TOKEN_KEYWORD_VACUUM,
    TOKEN_KEYWORD_ORDER,
    TOKEN_KEYWORD_ASC,
    TOKEN_KEYWORD_DESC,
    TOKEN_KEYWORD_LIKE,
    TOKEN_KEYWORD_AND,
    TOKEN_KEYWORD_OR,
    TOKEN_KEYWORD_LIMIT,
    TOKEN_KEYWORD_DISTINCT,
    TOKEN_KEYWORD_HAVING,
    TOKEN_KEYWORD_DROP,
    TOKEN_KEYWORD_DATABASE,
    TOKEN_KEYWORD_USE,
    TOKEN_KEYWORD_JOIN,
    TOKEN_KEYWORD_INNER,
    TOKEN_KEYWORD_AS,
    TOKEN_KEYWORD_BEGIN,
    TOKEN_KEYWORD_COMMIT,
    TOKEN_KEYWORD_ROLLBACK,
    TOKEN_KEYWORD_FLOAT,
    TOKEN_KEYWORD_DATE,
    TOKEN_KEYWORD_VARCHAR,
    TOKEN_KEYWORD_PRIMARY,
    TOKEN_KEYWORD_KEY,
    TOKEN_KEYWORD_UNIQUE,
    TOKEN_KEYWORD_DEFAULT,
    TOKEN_KEYWORD_CHECK,
    TOKEN_KEYWORD_ALTER,
    TOKEN_KEYWORD_ADD,
    TOKEN_KEYWORD_COLUMN,
    TOKEN_KEYWORD_RENAME,
    TOKEN_KEYWORD_TO,
    TOKEN_KEYWORD_LEFT,
    TOKEN_KEYWORD_OUTER,
    TOKEN_KEYWORD_IN,
    TOKEN_KEYWORD_EXISTS,

    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,

    TOKEN_STAR,
    TOKEN_COMMA,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_SEMICOLON,
    TOKEN_MINUS,
    TOKEN_PLUS,
    TOKEN_SLASH,
    TOKEN_PERCENT,

    TOKEN_OPERATOR_EQ,
    TOKEN_OPERATOR_NE,
    TOKEN_OPERATOR_LT,
    TOKEN_OPERATOR_LTE,
    TOKEN_OPERATOR_GT,
    TOKEN_OPERATOR_GTE,

    TOKEN_UNKNOWN
} TokenType;

typedef struct {
    TokenType   type;
    const char* value;              /* interned in the statement arena */
} Token;

typedef struct {
    Token tokens[MAX_TOKENS];
    int   count;
} TokenList;

/* ---------- values and rows ---------- */

/* A chunked bump allocator. Chunks are never reallocated, so a pointer handed
   out stays valid until the arena is released past it. */
typedef struct ArenaChunk ArenaChunk;

typedef struct {
    ArenaChunk* head;
} Arena;

typedef struct {                    /* a position to unwind an arena back to */
    ArenaChunk* chunk;
    size_t      used;
} ArenaMark;


/* Appended to, never reordered: the number is written into every record, so
   TYPE_INT must stay 0 and TYPE_TEXT 1 for older files to read back. */
typedef enum { TYPE_INT, TYPE_TEXT, TYPE_FLOAT, TYPE_DATE } ColType;

/* Column constraints. PRIMARY KEY is not a constraint of its own at execution
   time - it is UNIQUE and NOT NULL together, and the flag is kept only so that
   .tables can say which one the user actually wrote. */
#define COL_NOT_NULL 1
#define COL_UNIQUE   2
#define COL_PRIMARY  4

/* Text is not stored in the value. It points into an arena - the per-statement
   one for anything a query produced, or an index's own for a key that has to
   outlive the statement that inserted it. A Value therefore copies by
   assignment, the way the rest of this engine assumes, without carrying a
   kilobyte of mostly-unused buffer around with it. */
typedef struct {
    ColType     type;
    int         isNull;             /* 1 when this value is SQL NULL */
    int         intValue;
    double      floatValue;         /* TYPE_FLOAT only */
    const char* text;               /* NUL-terminated; may be NULL when unset */
    int         textLength;
} Value;

typedef struct {
    char    name[NAME_LEN];
    ColType type;
    int     size;                   /* varchar(n); 0 means unbounded text */
    int     flags;                  /* COL_NOT_NULL | COL_UNIQUE | COL_PRIMARY */
    int     hasDefault;
    Value   defaultValue;           /* text points into the catalog's own arena */
} Column;

/* days since 1970-01-01, which is how a DATE is stored and compared */
int  textToDate(const char* text, int length, int* days);
void dateToText(int days, char* out);           /* out holds 11 bytes */

typedef struct {
    Value values[MAX_COLS];
    int   ncols;
    int   deleted;                  /* tombstone: row is gone but keeps its position */
} Row;

/* ---------- expressions ---------- */

/*
 * An arithmetic expression over columns and literals: what a query means by
 * "price * qty" wherever a value can be read.
 *
 * The nodes live in a fixed pool with indices for edges, exactly as a Condition
 * does and for the same reason - the whole thing copies by value, needs no
 * allocation, and serialises as itself. A leaf is a column or a literal, and
 * every stage after the parser reaches one through the pool rather than through
 * a pointer.
 */
typedef enum { EXPR_COLUMN, EXPR_LITERAL, EXPR_BINARY, EXPR_NEGATE } ExprKind;

typedef enum { ARITH_ADD, ARITH_SUB, ARITH_MUL, ARITH_DIV, ARITH_MOD } ArithOp;

typedef struct {
    ExprKind kind;
    ArithOp  op;                    /* EXPR_BINARY only */
    int      left;                  /* pool indices, -1 when unused */
    int      right;
    char     column[NAME_LEN];      /* EXPR_COLUMN */
    Value    literal;               /* EXPR_LITERAL */
    int      slot;                  /* the column's position, once resolved */
    ColType  type;                  /* what this node produces, once resolved */
} ExprNode;

typedef struct {
    ExprNode nodes[MAX_EXPR_NODES];
    int      count;
} ExprPool;

/* ---------- parsed statements ---------- */

typedef enum {
    OP_EQ, OP_NE, OP_LT, OP_LTE, OP_GT, OP_GTE,
    OP_IS_NULL, OP_IS_NOT_NULL,         /* these two ignore Predicate.value */
    OP_LIKE, OP_NOT_LIKE,               /* text only, % and _ wildcards */
    /* Both read Predicate.subquery. Kept last so that the on-disk check in
       12_persist.c - which refuses an operator above OP_NOT_LIKE - still
       describes exactly the operators a stored CHECK may contain. */
    OP_IN,                              /* left IN (select ...) */
    OP_EXISTS                           /* EXISTS (select ...); no operands */
} CompareOp;

/*
 * Two expressions and an operator between them. Both sides are pool indices, so
 * "a = 1", "a = b" and "a * 2 > b + 1" are one shape rather than three - the
 * places that still care about the simple cases ask what an operand is
 * (exprIsColumn, exprIsLiteral) instead of reading a different field.
 *
 * The right side is unused by IS NULL and IS NOT NULL.
 */
typedef struct {
    int       left;
    CompareOp op;
    int       right;
    /* -1, or an index into the subquery pool. A subquery is uncorrelated, so
       it is run once before the outer scan starts and what it produced is what
       every row is then tested against. */
    int       subquery;
} Predicate;

typedef enum { COND_COMPARE, COND_AND, COND_OR, COND_NOT } ConditionKind;

typedef struct {
    ConditionKind kind;
    Predicate     compare;          /* COND_COMPARE only */
    int           left;             /* node indices into Condition.nodes, -1 unused */
    int           right;
} ConditionNode;

/* ponytail: nodes live in a fixed pool so a Condition copies by value and needs
   no allocation. 32 is far more than hand-written SQL reaches. */
typedef struct {
    int           present;          /* 0 when the statement had no WHERE */
    int           root;
    int           count;
    ConditionNode nodes[MAX_CONDITION_NODES];
    /* The operands of every predicate in this tree. Held here so a Condition
       is still one self-contained thing that copies, and so a CHECK carries
       its expressions into the catalog and onto the disk with it. */
    ExprPool      exprs;
} Condition;

typedef enum { AGG_NONE, AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX } AggregateType;

typedef struct {
    AggregateType aggregate;        /* AGG_NONE means no aggregate wrapping it */
    int           expr;             /* into SelectStatement.exprs; -1 for count(*) */
    int           star;             /* 1 for count(*) */
    char          label[NAME_LEN];  /* header text, e.g. "count(*)" or "a*2" */
    int           aliased;          /* 1 when AS named the label, so it wins */
} SelectItem;

typedef struct {
    char column[NAME_LEN];          /* matched against the result headers */
    int  descending;
} OrderTerm;

typedef struct {
    SelectItem items[MAX_COLS];
    ExprPool   exprs;               /* what the select list projects */
    int        nitems;
    int        selectAll;           /* 1 when the statement was SELECT * */
    char       table[NAME_LEN];     /* always tables[0]; the single-table paths read this */
    char       tables[MAX_JOIN_TABLES][NAME_LEN];
    /* What columns of that table are qualified by: the alias when the query
       gave one, otherwise the table name itself. */
    char       aliases[MAX_JOIN_TABLES][NAME_LEN];
    int        ntables;             /* > 1 means the FROM list is a join */
    /* LEFT JOIN. An inner join's ON is ANDed into the WHERE tree, because for
       an inner join filtering and pairing are the same thing. An outer join's
       is not: a row of the left table survives whether or not the ON matched,
       so the two have to be asked separately. The subtree still lives in
       where.nodes - only its root is held apart. */
    int        outer[MAX_JOIN_TABLES];      /* 1 when table t was LEFT JOINed */
    int        onRoot[MAX_JOIN_TABLES];     /* its ON tree, -1 when there is none */
    Condition  where;
    int        distinct;            /* SELECT DISTINCT */
    Condition  having;              /* names result columns, not table columns */
    OrderTerm  order[MAX_COLS];
    int        norder;
    int        limit;                /* -1 when the statement had no LIMIT */
    char       groupBy[MAX_COLS][NAME_LEN];
    int        ngroup;
} SelectStatement;

typedef struct {
    char      table[NAME_LEN];
    Column    cols[MAX_COLS];
    int       ncols;
    /* Every CHECK the statement wrote, column-level and table-level alike,
       ANDed into one tree - the same shape and the same evaluator a WHERE
       uses, because a CHECK is a WHERE that has to stay true. */
    Condition check;
} CreateStatement;

typedef struct {
    char  table[NAME_LEN];
    /* The columns the statement named, if it named any. Without a list the
       values are positional and there must be one for every column, which is
       what keeps a miscounted INSERT an error rather than a row of NULLs. */
    char  columns[MAX_COLS][NAME_LEN];
    int   ncolumns;
    Value values[MAX_COLS];
    int   nvalues;
} InsertStatement;

typedef struct {
    char name[NAME_LEN];
    char table[NAME_LEN];
    char column[NAME_LEN];
} CreateIndexStatement;

typedef struct {
    char      table[NAME_LEN];
    Condition where;
} DeleteStatement;

typedef struct {
    char column[NAME_LEN];
    int  expr;                      /* into UpdateStatement.exprs */
} Assignment;

typedef struct {
    char       table[NAME_LEN];
    Assignment sets[MAX_COLS];
    ExprPool   exprs;               /* what the SET clauses evaluate */
    int        nsets;
    Condition  where;
} UpdateStatement;

/* ALTER TABLE. The three that only touch names are separated from ADD and
   DROP COLUMN because those two have to rewrite every row: a record carries
   its own column count, so a row written before the change decodes with the
   old shape. */
typedef enum {
    ALTER_ADD_COLUMN, ALTER_DROP_COLUMN,
    ALTER_RENAME_COLUMN, ALTER_RENAME_TABLE
} AlterAction;

typedef struct {
    char        table[NAME_LEN];
    AlterAction action;
    Column      column;                 /* ADD COLUMN: the column to append */
    char        name[NAME_LEN];         /* DROP / RENAME COLUMN: which one */
    char        newName[NAME_LEN];      /* RENAME COLUMN / RENAME TO: the new name */
} AlterStatement;

typedef enum {
    STMT_SELECT, STMT_CREATE_TABLE, STMT_CREATE_INDEX,
    STMT_INSERT, STMT_DELETE, STMT_UPDATE, STMT_VACUUM,
    STMT_DROP_TABLE, STMT_DROP_INDEX,
    STMT_CREATE_DATABASE, STMT_USE_DATABASE, STMT_DROP_DATABASE,
    STMT_ALTER_TABLE,
    STMT_BEGIN, STMT_COMMIT, STMT_ROLLBACK      /* these carry nothing */
} StatementType;

typedef struct {
    StatementType type;
    union {
        SelectStatement select;
        CreateStatement create;
        InsertStatement insert;
        CreateIndexStatement createIndex;
        DeleteStatement del;            /* "delete" is a keyword in C++ headers */
        UpdateStatement update;
        AlterStatement  alter;
        char vacuumTable[NAME_LEN];     /* VACUUM carries nothing but a name */
        char databaseName[NAME_LEN];    /* CREATE DATABASE / USE target */
        char dropName[NAME_LEN];        /* DROP TABLE / DROP INDEX target */
    } u;
} Statement;

/* ---------- catalog ---------- */

typedef struct CatalogNode {
    char   table[NAME_LEN];
    Column cols[MAX_COLS];
    int    ncols;
    /* Allocated only when the table has a CHECK, because a Condition is some
       kilobytes and most tables have none. */
    Condition* check;
    struct CatalogNode* next;
} CatalogNode;

/* ---------- storage ---------- */

/* The unit the buffer pool moves between memory and the file. 8 KB is big
   enough that per-page overhead is noise and small enough that reading one to
   reach a single row is not wasteful. */
#define PAGE_SIZE 8192

/*
 * The last bytes of every page belong to the pool, not to whatever the page
 * holds: a checksum over everything before them, and four bytes spare. Every
 * layout that lives in a page - the heap's records, a B-tree node's entries,
 * the catalog chain's payload - stops short of them.
 *
 * A checksum of zero means the page was never stamped: a hole in the file, or
 * a page from a database written before there were checksums. Those are not
 * checked, because there is nothing to check them against.
 */
#define PAGE_TRAILER 8
#define PAGE_USABLE  (PAGE_SIZE - PAGE_TRAILER)

typedef struct Page {
    unsigned char data[PAGE_SIZE];
} Page;

/* A heap is a list of pages held by the pool, not by the heap itself. Rows are
   reached through heapRead rather than indexed directly, because a position
   addresses a slot inside a page, not an array element. Slot counts are
   mirrored here so that iterating never has to pin a page. */
typedef struct {
    char table[NAME_LEN];
    int* pageIds;                   /* pool page id for each local page */
    int* pageSlots;                 /* slots used in each, tombstones included */
    int  npages;
    int  capacity;
    int  nlive;                     /* live rows */
    int  nslots;                    /* slots ever allocated */
} Heap;

/*
 * A walk over a heap that keeps the page it is on pinned, so a record can be
 * read where it lies instead of being copied out. One pin per page rather than
 * one per row, and no text is copied at all - which is what makes a scan that
 * throws most rows away cheap.
 *
 * The record and anything decoded from it are valid only until the next call,
 * because that is when the page may be let go.
 */
typedef struct {
    const Heap*   heap;
    int           page;             /* index into the heap's page list */
    int           slot;
    int           pageId;           /* the pinned page, -1 when none is */
    struct Page*  pinned;
} HeapScan;

/* ---------- indexes ---------- */

/* A B+ tree whose nodes are pool pages; see 11_index.c for the node layout.
   Only the root page id lives here, so a tree is saved and reopened with the
   rest of the pages instead of being rebuilt by scanning the table. */
typedef struct Index {
    char   name[NAME_LEN];
    char   table[NAME_LEN];
    char   column[NAME_LEN];
    int    slot;                            /* column position in the row */
    int    rootPage;
    int    keyCount;                        /* kept as we go; .indexes is O(1) */
    struct Index* next;
} Index;

/* ---------- results ---------- */

typedef struct {
    char headers[MAX_COLS][NAME_LEN];
    /* What each column holds, which the rows cannot always say: a result with
       no rows in it still has types, and a client asking what a query returns
       is asking before there are any. */
    ColType types[MAX_COLS];
    int  ncols;
    Row* rows;                      /* grown on demand by resultReserve */
    int  nrows;
    int  capacity;
    int  rowsAffected;              /* for INSERT / CREATE */
    char message[VALUE_LEN];        /* set when there is no row output */
} ResultSet;

/* ---------- 01 errors ---------- */
const char* errorCodeToString(int errorCode);

/* ---------- 02 catalog ---------- */
void         initCatalog(void);
CatalogNode* findTable(const char* name);
int          addTable(const char* name, const Column cols[], int ncols,
                      const Condition* check);
int          findColumn(const CatalogNode* table, const char* name);
const char*  exprUnresolvedColumn(void);
int          renameTableInCatalog(const char* from, const char* to);

/* ---------- 04 parser: the subquery pool ---------- */
void             resetSubqueries(void);
int              subqueryTotal(void);
int              subqueryIsScalar(int index);
SelectStatement* subqueryAt(int index);
int          buildJoinSchema(const char tables[][NAME_LEN],
                             const char aliases[][NAME_LEN], int ntables,
                             CatalogNode* out);
int          joinSchemaNeeded(const SelectStatement* select);
int          dropTable(const char* name);
int          listTables(const CatalogNode** out, int max);
void         freeCatalog(void);
void         printCatalog(void);

/* ---------- 03 lexer ---------- */
int tokenizeStatement(const char* sql, TokenList* out);

/* ---------- 04 parser ---------- */
int parseStatement(const TokenList* tokens, Statement* out);

/* ---------- 17 expressions ---------- */
void        exprInit(ExprPool* pool);
int         exprNew(ExprPool* pool, ExprKind kind, int* node);
int         exprIsColumn(const ExprPool* pool, int node);
int         exprIsLiteral(const ExprPool* pool, int node);
const char* exprColumn(const ExprPool* pool, int node);
Value*      exprLiteral(ExprPool* pool, int node);
void        exprLabel(const ExprPool* pool, int node, char* out, size_t size);
int         exprResolve(const CatalogNode* table, ExprPool* pool, int node);
int         exprType(const ExprPool* pool, int node, ColType* out);
int         exprDeepest(const ExprPool* pool, int node);
int         exprEvaluate(const ExprPool* pool, int node, const Row* row, Value* out);

/* ---------- 05 semantic ---------- */
/* Takes a mutable statement because checking is also where a literal is fitted
   to the column it is going to meet: '2024-05-01' becomes a date, 3 becomes
   3.0 for a float column. Nothing below this knows about conversion. */
int semanticCheck(Statement* statement);
int coerceLiteral(Value* value, ColType type);

/* ---------- 06 executor ---------- */
int executeStatement(Statement* statement, ResultSet* out);
void freeExecutor(void);
void setExplain(int on);

/* ---------- 07 storage ---------- */
void  initStorage(void);
Heap* findHeap(const char* table);
void  renameHeap(const char* from, const char* to);
Heap* createHeap(const char* table);
int   heapInsert(Heap* heap, const Row* row, int* position);
int   heapRead(const Heap* heap, int position, Row* out);
int   heapMarkDeleted(Heap* heap, int position);
int   heapCompact(Heap* heap, int* reclaimed);
void  heapReset(Heap* heap);
int   heapAdoptPage(Heap* heap, int pageId, int slotsUsed);
void  heapSetLive(Heap* heap, int live);
int   heapPageCount(const Heap* heap);
int   heapPageId(const Heap* heap, int index);
int   heapPageSlots(const Heap* heap, int index);
int   heapSlots(const Heap* heap);
int   heapFirst(const Heap* heap);
int   heapNext(const Heap* heap, int position);
int   heapLive(const Heap* heap);

void  heapScanStart(HeapScan* scan, const Heap* heap);
int   heapScanNext(HeapScan* scan, int* position, const unsigned char** record);
void  heapScanEnd(HeapScan* scan);
const unsigned char* heapScanAt(HeapScan* scan, int position);
void  decodeRecord(const unsigned char* at, Row* out, int upto);
int   dropHeap(const char* table);
void  freeStorage(void);

/* ---------- 14 buffer pool ---------- */
void  poolInit(void);
void  poolClear(void);
void  poolAdopt(FILE* file, int pages);
void  poolDetachFile(void);
void  poolMarkAllClean(void);
int   poolIsWritable(void);
int   poolPageCount(void);
Page* poolPin(int pageId);
void  poolUnpin(int pageId, int dirty);
int   poolAllocate(int* pageId);
void  poolFree(int pageId);
int   poolWriteAll(FILE* file);
int   poolCommit(void);
int   poolHasDirty(void);
long  poolCorruptCount(void);
void  poolRollback(int pages);
int   poolCheckpoint(void);
void  poolSetWritable(FILE* file, int pages);
void  poolSetChecksums(int on);
void  poolStampPage(Page* page);
void  poolSetCatalogRoot(int page);
int   poolCatalogRoot(void);
void  poolReport(void);

/* ---------- 15 write-ahead log ---------- */
int  walOpen(const char* dbPath);
void walClose(void);
int  walIsOpen(void);
long walFrameCount(void);
int  walAppend(int pageId, const Page* page, int commit,
               int pageCount, int catalogRoot);
int  walSync(void);
int  walTruncate(void);
int  walRecover(const char* dbPath, int* recovered);
int  walSyncHandle(FILE* file);
void walDiscard(const char* dbPath);
int  isPageFile(const char* path);

/* ---------- 18 threads ---------- */

/* What a lock is made of differs per platform, so 18_thread.c keeps both the
   types and the two locks to itself. Everything else asks for the engine lock
   by name and never holds one of its own. */
typedef void* (*ThreadFunction)(void*);

int  threadStart(ThreadFunction function, void* argument);

void initThreads(void);
void freeThreads(void);

/* Held for one statement: shared by anything that only reads, exclusive by
   anything that writes. No-ops until a server starts one, so a plain REPL
   session pays nothing for them. */
void engineReadLock(void);
void engineWriteLock(void);
void engineReadUnlock(void);
void engineWriteUnlock(void);

/* Taken underneath the engine lock, never the other way round - which is the
   whole of the lock ordering rule, because there are only these two. */
/* This session's own answer to "am I in a transaction", which is not the
   engine's: only one connection can be, and it is the one holding the lock. */
int  engineInTransaction(void);
void engineReleaseTransaction(void);

/* Room for one more connection, or not. The count is the server's, not the
   engine's, so it has a lock of its own and never waits behind a statement. */
int  sessionAcquire(int limit);
void sessionRelease(void);

void poolEnter(void);
void poolLeave(void);

/* ---------- 11 index ---------- */
void   initIndexes(void);
Index* findIndexByName(const char* name);
Index* findIndexOn(const char* table, const char* column);
int    createIndex(const char* name, const char* table, const char* column,
                  int slot, ColType keyType);
int    adoptIndex(const char* name, const char* table, const char* column,
                  int slot, int rootPage, int keyCount);
int    indexRootPage(const Index* index);
int    indexKeyCount(const Index* index);
int    indexInsert(Index* index, const Value* key, int rowPosition);
int    indexInsertRow(const char* table, const Row* row, int rowPosition);
int    rebuildIndexes(const char* table, const Heap* heap);
void   indexesColumnDropped(const char* table, const char* column, int slot);
void   indexesColumnRenamed(const char* table, const char* from, const char* to);
void   indexesTableRenamed(const char* from, const char* to);
int    dropIndexByName(const char* name);
void   dropIndexesForTable(const char* table);
int    listIndexes(const Index** out, int max);
int    indexPrefixScan(const Index* index, const char* prefix,
                       int* rows, int maxRows, int* nrows);
int    indexScan(const Index* index, CompareOp op, const Value* key,
                 int* rows, int maxRows, int* nrows);
int    indexableOperator(CompareOp op);
void   freeIndexes(void);
void   printIndexes(void);

/* ---------- text arena and value helpers (07 storage) ---------- */

const char* arenaCopy(Arena* arena, const char* text, int length);
void        arenaRelease(Arena* arena);

/* The statement arena: everything a query reads or parses lands here, and it is
   emptied when the next statement begins. */
const char* internText(const char* text, int length);
void        resetTextArena(void);
ArenaMark   textMark(void);
void        textReset(ArenaMark mark);

int  compareValues(const Value* a, const Value* b);
int  valuesEqual(const Value* a, const Value* b);
void setNull(Value* value, ColType type);
void setFloat(Value* value, double number);
void setText(Value* value, const char* text, int length);
const char* valueText(const Value* value);
int         valuesSame(const Value* a, const Value* b);

/* ---------- 13 databases ---------- */
void        initDatabases(void);
int         createDatabase(const char* name);
int         useDatabase(const char* name);
int         findDatabase(const char* name);
int         currentDatabaseId(void);
void        setCurrentDatabaseId(int id);
const char* currentDatabaseName(void);
int         databaseCount(void);
int         databaseSlotCount(void);
int         databaseInUse(int id);
void        releaseDatabase(int id);
const char* databaseName(int id);
void        selectDatabaseById(int id);
void        printDatabases(void);

/* ---------- 12 persistence ---------- */
int saveDatabase(const char* path);
const char* databasePath(void);
int  openForWrite(const char* path);
int  commitDatabase(void);
void markSchemaChanged(void);
int  beginTransaction(void);
int  commitTransaction(void);
int  rollbackTransaction(void);
void abandonTransaction(void);
int  inTransaction(void);
int  closeDatabase(void);
int loadDatabase(const char* path);

/* ---------- 08 result set ---------- */
void printResultSet(const ResultSet* results);
int  resultReserve(ResultSet* results, int rows);
void freeResultSet(ResultSet* results);

/* ---------- 09 view ---------- */
void showBanner(void);
void showError(int errorCode);

/* ---------- 16 postgres wire protocol ---------- */
int serveWire(int port);

/* ---------- 10 controller ---------- */
void InitEngine(void);
int  ProcessStatement(const char* sql, ResultSet* out);
void FreeEngine(void);

#endif
