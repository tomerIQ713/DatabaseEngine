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
/*
 * What a statement needs from the engine lock.
 *
 * A SELECT changes no data, so any number of them run together. Everything
 * else - including BEGIN, which is about to make changes - runs alone. The
 * question is asked of the parsed statement rather than the text, because
 * "select" is not the only word a statement can start with and guessing from
 * the front of a string is exactly the kind of thing that is wrong once.
 */
static int readsOnly(StatementType type)
{
    return type == STMT_SELECT;
}

/*
 * Whether this thread is holding the write lock *between* statements, which is
 * what an open transaction means: BEGIN takes it and does not give it back
 * until COMMIT or ROLLBACK, because a transaction that let other statements in
 * between its own would not be one.
 *
 * Thread-local, so it is the session's own answer and not the engine's.
 */
static THREAD_LOCAL int holdingForTransaction;

int engineInTransaction(void)
{
    return holdingForTransaction;
}

void engineReleaseTransaction(void)
{
    if (!holdingForTransaction)
        return;

    /* The caller has already rolled back under the lock it still holds. */
    holdingForTransaction = ZERO;
    engineWriteUnlock();
}

int ProcessStatement(const char* sql, ResultSet* out)
{
    TokenList tokens;
    Statement statement;
    int errorCode;

    /* Everything this statement parses or reads is interned here, and none of
       it outlives the result set the caller is about to print. Both are
       thread-local, so this is the calling thread's own arena and its own
       subqueries. */
    resetTextArena();
    resetSubqueries();

    out->ncols = ZERO;
    out->nrows = ZERO;
    out->rowsAffected = ZERO;
    out->message[ZERO] = '\0';

    /* Tokenising and parsing touch nothing shared - the token list and the
       statement are on this thread's stack, the subquery pool and the text
       arena are its own - so they happen before the lock is taken. */
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

    /* Inside a transaction the lock is already held, and exclusively. */
    int shared = !holdingForTransaction && readsOnly(statement.type);

    if (!holdingForTransaction) {
        if (shared)
            engineReadLock();
        else
            engineWriteLock();
    }

    errorCode = semanticCheck(&statement);

    if (errorCode == SUCCESS_CODE) {
        /*
         * A page that fails its checksum is refused, and every caller in the
         * engine treats a page it cannot have as one that holds nothing -
         * which would make a damaged file look like an empty table. Noticing
         * here turns that back into what it is.
         */
        long damaged = poolCorruptCount();

        errorCode = executeStatement(&statement, out);

        if (errorCode == SUCCESS_CODE && poolCorruptCount() != damaged)
            errorCode = ERROR_IO_CHECKSUM;

        if (errorCode == SUCCESS_CODE) {
            /* A schema change need not touch a single page - CREATE TABLE with
               no rows does not - and the commit below stops early when nothing
               is dirty. Said once here rather than in each of the statements,
               because the cost of missing one is a change that reported
               success and was not kept. */
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

            /* The statement worked, so make it durable before anyone is told
               so - unless it is part of a transaction, which is exactly the
               promise BEGIN makes: one fsync at COMMIT rather than one per
               statement.
               A reader never commits, and not only because it has nothing to
               make durable: it holds the engine lock *shared*, so several are
               running, and committing writes the catalog and appends to the
               log. The pages a SELECT leaves dirty belong to a join's
               synthetic heap - scratch, which the next writer will carry out
               with its own commit. */
            if (!shared && !inTransaction())
                errorCode = commitDatabase();
        }
    }

    /*
     * A transaction keeps the write lock; anything else gives back what it
     * took. Asked after the statement has run, because BEGIN is the statement
     * that starts holding it and COMMIT is the one that stops.
     */
    if (holdingForTransaction) {
        if (!inTransaction()) {
            holdingForTransaction = ZERO;
            engineWriteUnlock();
        }
    }
    else if (shared) {
        engineReadUnlock();
    }
    else if (inTransaction()) {
        holdingForTransaction = ONE;        /* kept until COMMIT or ROLLBACK */
    }
    else {
        engineWriteUnlock();
    }

    return errorCode;
}
