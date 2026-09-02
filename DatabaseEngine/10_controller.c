#include "sql_common.h"

void InitEngine(void)
{
    poolInit();
    initDatabases();
    initCatalog();
    initStorage();
    initIndexes();
}

void FreeEngine(void)
{
    freeExecutor();
    freeIndexes();
    freeCatalog();
    freeStorage();
    poolClear();
}

/*
 * One statement through the whole pipeline. Mirrors ProcessTokenLine,
 * with execution on the end.
 */
int ProcessStatement(const char* sql, ResultSet* out)
{
    TokenList tokens;
    Statement statement;
    int errorCode;

    /* Everything this statement parses or reads is interned here, and none of
       it outlives the result set the caller is about to print. */
    resetTextArena();
    resetSubqueries();

    out->ncols = ZERO;
    out->nrows = ZERO;
    out->rowsAffected = ZERO;
    out->message[ZERO] = '\0';

    errorCode = tokenizeStatement(sql, &tokens);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /* A line that was nothing but a comment leaves no tokens, and an empty
       result set prints nothing - so it costs one check to be a no-op. */
    if (tokens.count == ZERO)
        return SUCCESS_CODE;

    errorCode = parseStatement(&tokens, &statement);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    errorCode = semanticCheck(&statement);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /*
     * A page that fails its checksum is refused, and every caller in the engine
     * treats a page it cannot have as one that holds nothing - which would make
     * a damaged file look like an empty table. Noticing here turns that back
     * into what it is.
     */
    long damaged = poolCorruptCount();

    errorCode = executeStatement(&statement, out);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /* A schema change need not touch a single page - CREATE TABLE with no rows
       does not - and the commit below stops early when nothing is dirty. Said
       once here rather than in each of the statements, because the cost of
       missing one is a change that reported success and was not kept. */
    switch (statement.type) {
    case STMT_CREATE_TABLE:  case STMT_DROP_TABLE:
    case STMT_CREATE_INDEX:  case STMT_DROP_INDEX:
    case STMT_ALTER_TABLE:   case STMT_VACUUM:
    case STMT_CREATE_DATABASE: case STMT_DROP_DATABASE:
        markSchemaChanged();
        break;
    default:
        break;
    }

    if (poolCorruptCount() != damaged)
        return ERROR_IO_CHECKSUM;

    /* The statement worked, so make it durable before anyone is told so -
       unless it is part of a transaction, which is exactly the promise BEGIN
       makes: one fsync at COMMIT rather than one per statement. */
    return inTransaction() ? SUCCESS_CODE : commitDatabase();
}
