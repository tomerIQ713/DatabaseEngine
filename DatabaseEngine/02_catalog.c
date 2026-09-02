#include "sql_common.h"
#include <ctype.h>

/* One bucket array per database; currentDatabaseId() picks the namespace. */
static CatalogNode* catalog[MAX_DATABASES][CATALOG_SIZE];

/*
 * Text belonging to the catalog itself - a DEFAULT literal, a literal inside a
 * CHECK - lives here rather than in the statement arena, which is emptied
 * before the next statement runs. A table outlives the CREATE that made it, so
 * everything the catalog keeps has to be copied out of that arena on the way
 * in. Released with the catalog.
 */
static Arena catalogArena;

static void keepValue(Value* value)
{
    if (!value->isNull && value->type == TYPE_TEXT && value->text != NULL)
        value->text = arenaCopy(&catalogArena, value->text, value->textLength);
}

static unsigned int hashName(const char* name)
{
    /* lowercased: findTable compares with _stricmp, so the hash must ignore case too */
    unsigned int h = 5381;
    while (*name)
        h = h * 33 + (unsigned char)tolower((unsigned char)(*name++));
    return h % CATALOG_SIZE;
}

void initCatalog(void)
{
    for (int d = ZERO; d < MAX_DATABASES; d++)
        for (int i = ZERO; i < CATALOG_SIZE; i++)
            catalog[d][i] = NULL;
}

CatalogNode* findTable(const char* name)
{
    CatalogNode** buckets = catalog[currentDatabaseId()];

    for (CatalogNode* node = buckets[hashName(name)]; node != NULL; node = node->next)
        if (_stricmp(node->table, name) == ZERO)
            return node;
    return NULL;
}

/*
 * Registers a table. Caller checks for duplicates in the semantic stage.
 */
int addTable(const char* name, const Column cols[], int ncols,
             const Condition* check)
{
    if (ncols <= ZERO || ncols > MAX_COLS)
        return ERROR_SYNTAX_TOO_MANY_COLUMNS;

    CatalogNode* node = (CatalogNode*)malloc(sizeof(CatalogNode));
    if (node == NULL)
        return GENERAL_ERROR;

    snprintf(node->table, NAME_LEN, "%s", name);
    memcpy(node->cols, cols, (size_t)ncols * sizeof(Column));
    node->ncols = ncols;
    node->check = NULL;

    for (int c = ZERO; c < ncols; c++)
        if (node->cols[c].hasDefault)
            keepValue(&node->cols[c].defaultValue);

    /* A Condition is some kilobytes, and most tables have no CHECK, so one is
       allocated only when there is something to hold. */
    if (check != NULL && check->present) {
        node->check = (Condition*)malloc(sizeof(Condition));

        if (node->check == NULL) {
            free(node);
            return ERROR_EXEC_OUT_OF_MEMORY;
        }

        *node->check = *check;

        /* Literals inside the CHECK live in the expression pool now. */
        for (int i = ZERO; i < node->check->exprs.count; i++)
            if (node->check->exprs.nodes[i].kind == EXPR_LITERAL)
                keepValue(&node->check->exprs.nodes[i].literal);
    }

    CatalogNode** buckets = catalog[currentDatabaseId()];
    unsigned int  slot    = hashName(name);

    node->next    = buckets[slot];
    buckets[slot] = node;

    /* Bind the CHECK to this table once, here, rather than on every row it is
       later asked about - including a CHECK that arrived from a saved file and
       has never been resolved against anything. */
    if (node->check != NULL)
        for (int i = ZERO; i < node->check->count; i++)
            if (node->check->nodes[i].kind == COND_COMPARE) {
                exprResolve(node, &node->check->exprs,
                            node->check->nodes[i].compare.left);
                exprResolve(node, &node->check->exprs,
                            node->check->nodes[i].compare.right);
            }

    return SUCCESS_CODE;
}

/*
 * Returns the column's position in the table, or -1 if absent.
 */
/* the part after the last dot, or the whole name when there is none */
static const char* bareName(const char* name)
{
    const char* dot = strrchr(name, '.');
    return dot != NULL ? dot + ONE : name;
}

/*
 * Exact match first, which is all a single-table query ever needs.
 *
 * The fallbacks exist because a reference and a column can name the same thing
 * with different amounts of qualification. In a join schema the columns are
 * "users.id" and a query may say plain "id"; in a single table the column is
 * "id" and a query may say "users.id". Neither is resolved by guessing: a bare
 * name matching two joined tables is reported as ambiguous, and a qualified
 * name only resolves against the table it actually names.
 */
int findColumn(const CatalogNode* table, const char* name)
{
    for (int i = ZERO; i < table->ncols; i++)
        if (_stricmp(table->cols[i].name, name) == ZERO)
            return i;

    const char* dot = strrchr(name, '.');

    if (dot != NULL) {
        /* "users.id" against the columns of users, which are stored unqualified */
        size_t length = (size_t)(dot - name);

        if (_strnicmp(name, table->table, length) != ZERO
            || table->table[length] != '\0')
            return -1;

        for (int i = ZERO; i < table->ncols; i++)
            if (_stricmp(table->cols[i].name, dot + ONE) == ZERO)
                return i;

        return -1;
    }

    int found = -1;                             /* bare name against a join schema */

    for (int i = ZERO; i < table->ncols; i++)
        if (_stricmp(bareName(table->cols[i].name), name) == ZERO) {
            if (found >= ZERO)
                return COLUMN_AMBIGUOUS;
            found = i;
        }

    return found;
}

/*
 * Flattens the tables of a FROM list into one schema whose columns are named
 * "table.column". Joined rows are built to line up with it, which is what lets
 * every stage after the join go on treating the query as single-table.
 */
int buildJoinSchema(const char tables[][NAME_LEN], const char aliases[][NAME_LEN],
                    int ntables, CatalogNode* out)
{
    out->ncols = ZERO;
    out->next  = NULL;
    snprintf(out->table, NAME_LEN, "%s", JOIN_SCHEMA_NAME);

    for (int t = ZERO; t < ntables; t++) {
        /* Aliases are what columns get qualified by, so two of them colliding
           makes half the schema unaddressable - including "from a, a", where
           both default to the table name. */
        for (int prior = ZERO; prior < t; prior++)
            if (_stricmp(aliases[prior], aliases[t]) == ZERO)
                return ERROR_SEMANTIC_DUPLICATE_TABLE;

        const CatalogNode* source = findTable(tables[t]);
        if (source == NULL)
            return ERROR_SEMANTIC_TABLE_NOT_FOUND;

        for (int c = ZERO; c < source->ncols; c++) {
            if (out->ncols == MAX_COLS)
                return ERROR_SEMANTIC_JOIN_TOO_WIDE;

            /* The whole column, so a varchar limit and a date type survive the
               flattening - but not its constraints: nothing is inserted into a
               join, and a UNIQUE that no longer has an index behind it would
               be a promise this schema cannot keep. */
            Column* into = &out->cols[out->ncols];

            *into = source->cols[c];
            into->flags      = ZERO;
            into->hasDefault = ZERO;

            snprintf(into->name, NAME_LEN, "%.*s.%s",
                     NAME_LEN / TWO, aliases[t], source->cols[c].name);
            out->ncols++;
        }
    }

    return SUCCESS_CODE;
}

/*
 * A single unaliased table is resolved directly against the catalog, which is
 * both cheaper and what makes plain queries print unqualified headers. Anything
 * else - a join, or one table wearing an alias - goes through the flattened
 * schema, because that is the only place the alias exists.
 */
int joinSchemaNeeded(const SelectStatement* select)
{
    if (select->ntables > ONE)
        return ONE;

    return _stricmp(select->aliases[ZERO], select->tables[ZERO]) != ZERO;
}

/*
 * Unlinks a table from the catalog. Storage and indexes are dropped separately.
 */
int dropTable(const char* name)
{
    CatalogNode** buckets = catalog[currentDatabaseId()];
    CatalogNode** link    = &buckets[hashName(name)];

    while (*link != NULL) {
        if (_stricmp((*link)->table, name) == ZERO) {
            CatalogNode* dead = *link;
            *link = dead->next;
            free(dead->check);
            free(dead);
            return SUCCESS_CODE;
        }
        link = &(*link)->next;
    }

    return ERROR_SEMANTIC_TABLE_NOT_FOUND;
}

/*
 * Renames a table. The catalog is a hash table keyed on the name, so the node
 * has to leave one bucket and join another - changing the name in place would
 * leave it where findTable can no longer reach it.
 */
int renameTableInCatalog(const char* from, const char* to)
{
    if (findTable(to) != NULL)
        return ERROR_SEMANTIC_TABLE_EXISTS;

    CatalogNode** buckets = catalog[currentDatabaseId()];
    CatalogNode** link    = &buckets[hashName(from)];

    while (*link != NULL) {
        if (_stricmp((*link)->table, from) == ZERO) {
            CatalogNode* node = *link;

            *link = node->next;                 /* out of the old bucket */
            snprintf(node->table, NAME_LEN, "%s", to);

            unsigned int bucket = hashName(to);   /* and into the new one */
            node->next        = buckets[bucket];
            buckets[bucket]   = node;
            return SUCCESS_CODE;
        }
        link = &(*link)->next;
    }

    return ERROR_SEMANTIC_TABLE_NOT_FOUND;
}

/*
 * Collects every table in the current database. Order follows the hash buckets.
 */
int listTables(const CatalogNode** out, int max)
{
    CatalogNode** buckets = catalog[currentDatabaseId()];
    int           count   = ZERO;

    for (int i = ZERO; i < CATALOG_SIZE && count < max; i++)
        for (const CatalogNode* node = buckets[i]; node != NULL && count < max;
             node = node->next)
            out[count++] = node;

    return count;
}

/*
 * Frees every catalog entry in every database.
 */
void freeCatalog(void)
{
    for (int d = ZERO; d < MAX_DATABASES; d++)
        for (int i = ZERO; i < CATALOG_SIZE; i++) {
            CatalogNode* node = catalog[d][i];

            while (node != NULL) {
                CatalogNode* next = node->next;
                free(node->check);
                free(node);
                node = next;
            }
            catalog[d][i] = NULL;
        }

    arenaRelease(&catalogArena);
}

void printCatalog(void)
{
    CatalogNode** buckets = catalog[currentDatabaseId()];

    printf("--- catalog (%s) ---\n", currentDatabaseName());

    for (int i = ZERO; i < CATALOG_SIZE; i++)
        for (CatalogNode* node = buckets[i]; node != NULL; node = node->next) {
            printf("%s(", node->table);

            for (int c = ZERO; c < node->ncols; c++) {
                const Column* column = &node->cols[c];

                printf("%s%s ", c ? ", " : "", column->name);

                if (column->type == TYPE_TEXT && column->size > ZERO)
                    printf("varchar(%d)", column->size);
                else
                    printf("%s", column->type == TYPE_INT   ? "int"
                               : column->type == TYPE_FLOAT ? "float"
                               : column->type == TYPE_DATE  ? "date" : "text");

                if (column->flags & COL_PRIMARY)
                    printf(" primary key");
                else {
                    if (column->flags & COL_NOT_NULL)
                        printf(" not null");
                    if (column->flags & COL_UNIQUE)
                        printf(" unique");
                }

                if (column->hasDefault)
                    printf(" default");
            }

            printf(")%s\n", node->check != NULL ? " check" : "");
        }
}
