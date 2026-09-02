#include "sql_common.h"

/* Losing data by default is the wrong default for a database, so a bare run
   opens this file. Pass :memory: when a throwaway session is what you want. */
#define DEFAULT_DATABASE "database.db"
#define MEMORY_DATABASE  ":memory:"

/* The file this session writes to at exit, and whether a log is attached to
   it. `.load` moves both, so they cannot be locals of main. */
static char sessionPath[LINE_LEN];
static int  usingLog;

/*
 * Attaches the log, if this file is one that may be written in place. An older
 * format is not, so it is rewritten whole at exit instead.
 */
static void attachLog(const char* path, int current)
{
    usingLog = ZERO;

    /* A file this session has just written is current by construction; one it
       read is only current if the load left it as the file being paged from. */
    if (!current && databasePath() == NULL)
        return;

    /* And only a file of the current version may be written a page at a time.
       An older one is read happily, but its catalog does not have room for
       everything this version records - so writing today's catalog into
       yesterday's file would leave a file that says it is one thing and holds
       another. It is rewritten whole at exit instead, which upgrades it. */
    if (!isPageFile(path))
        return;

    int errorCode = openForWrite(path);

    if (errorCode == SUCCESS_CODE)
        usingLog = ONE;
    else
        showError(errorCode);
}

static void runFileCommand(int loading, const char* path)
{
    char kept[LINE_LEN];

    /* loadDatabase closes the log, which is what makes this safe. */
    snprintf(kept, sizeof kept, "%s", path);

    int errorCode = loading ? loadDatabase(kept) : saveDatabase(kept);

    if (errorCode != SUCCESS_CODE) {
        showError(errorCode);
        return;
    }

    printf("%s %s\n", loading ? "loaded" : "saved to", kept);

    /* A load moves the session onto the file it just read, log and all. */
    if (loading && sessionPath[ZERO] != '\0') {
        snprintf(sessionPath, sizeof sessionPath, "%s", kept);
        attachLog(kept, ZERO);
    }
}

/* what a dot-command asked the loop to do next */
typedef enum { NOT_A_COMMAND, HANDLED, QUIT } CommandResult;

/*
 * Dot-commands dispatch on their first four bytes rather than walking a chain of
 * strcmp. The bytes go into an integer with a fixed-size memcpy and the switch
 * over it compiles to a jump table, so anything unrecognised is rejected in one
 * dispatch. The prefixes are all distinct - ".exi" and ".exp" differ in the
 * fourth byte - so a case names one command and the compare that follows only
 * has to confirm the rest of it.
 *
 * No case folding here, unlike the lexer: SQL keywords are case-insensitive and
 * these are not.
 */
static CommandResult runDotCommand(const char* line)
{
    size_t length = strlen(line);

    if (line[ZERO] != '.')
        return NOT_A_COMMAND;                       /* ordinary SQL */

    if (length >= 4) {
        uint32_t prefix;
        memcpy(&prefix, line, sizeof prefix);

        switch (prefix) {
        case PACK_4('.', 'e', 'x', 'i'):
            if (strcmp(line, ".exit") == ZERO)
                return QUIT;
            break;

        case PACK_4('.', 'q', 'u', 'i'):
            if (strcmp(line, ".quit") == ZERO)
                return QUIT;
            break;

        case PACK_4('.', 't', 'a', 'b'):
            if (strcmp(line, ".tables") == ZERO) {
                printCatalog();
                return HANDLED;
            }
            break;

        case PACK_4('.', 'i', 'n', 'd'):
            if (strcmp(line, ".indexes") == ZERO) {
                printIndexes();
                return HANDLED;
            }
            break;

        case PACK_4('.', 'd', 'a', 't'):
            if (strcmp(line, ".databases") == ZERO) {
                printDatabases();
                return HANDLED;
            }
            break;

        case PACK_4('.', 'p', 'o', 'o'):
            if (strcmp(line, ".pool") == ZERO) {
                poolReport();
                return HANDLED;
            }
            break;

        case PACK_4('.', 'e', 'x', 'p'):
            if (strcmp(line, ".explain on") == ZERO) {
                setExplain(ONE);
                return HANDLED;
            }
            if (strcmp(line, ".explain off") == ZERO) {
                setExplain(ZERO);
                return HANDLED;
            }
            break;

        case PACK_4('.', 's', 'a', 'v'):
            if (strncmp(line, ".save ", 6) == ZERO && line[6] != '\0') {
                runFileCommand(ZERO, line + 6);
                return HANDLED;
            }
            break;

        case PACK_4('.', 'l', 'o', 'a'):
            if (strncmp(line, ".load ", 6) == ZERO && line[6] != '\0') {
                runFileCommand(ONE, line + 6);
                return HANDLED;
            }
            break;

        default:
            break;                                  /* falls through to the report */
        }
    }

    /* A leading dot was a command that did not land, so say so rather than
       handing it to the parser and calling it a syntax error. */
    printf("unrecognised command: %s\n", line);
    return HANDLED;
}

int main(int argc, char** argv)
{
    char line[LINE_LEN];
    /* Rows are allocated on demand now, so this is small - but it has to start
       zeroed for the first resultReserve to see an empty buffer. */
    ResultSet results = { ZERO };

    const char* databaseFile = DEFAULT_DATABASE;
    int         port         = ZERO;

    /* db [database] [--port n]. With a port it speaks the PostgreSQL protocol
       instead of reading lines, and psql is the client. */
    for (int i = ONE; i < argc; i++) {
        if (strcmp(argv[i], "--port") == ZERO && i + ONE < argc) {
            port = atoi(argv[++i]);
            continue;
        }
        databaseFile = argv[i];
    }

    if (strcmp(databaseFile, MEMORY_DATABASE) == ZERO)
        databaseFile = NULL;                            /* nothing is written */

    InitEngine();
    showBanner();

    if (databaseFile == NULL) {
        printf("in-memory only, nothing will be saved\n");
    }
    else {
        /* A log left behind means the last session did not finish. Replaying it
           before anything is read is what makes the file whole again. */
        int recovered = ZERO;
        int errorCode = walRecover(databaseFile, &recovered);

        if (errorCode != SUCCESS_CODE)
            showError(errorCode);
        else if (recovered > ZERO)
            printf("recovered %d page(s) from the log\n", recovered);

        int fresh = ZERO;

        errorCode = loadDatabase(databaseFile);

        if (errorCode == ERROR_IO_CANNOT_OPEN) {        /* no file yet: make one */
            errorCode = saveDatabase(databaseFile);

            if (errorCode == SUCCESS_CODE) {
                printf("new database %s\n", databaseFile);
                fresh = ONE;
            }
            else {
                showError(errorCode);
                databaseFile = NULL;
            }
        }
        else if (errorCode == SUCCESS_CODE) {
            printf("loaded %s\n", databaseFile);
        }
        else {
            showError(errorCode);                       /* corrupt: do not overwrite */
            databaseFile = NULL;                        /* and do not write to it */
        }

        if (databaseFile != NULL) {
            snprintf(sessionPath, sizeof sessionPath, "%s", databaseFile);
            attachLog(databaseFile, fresh);
        }
    }

    /* Serving a socket and reading lines are the same session with a different
       front door, so this sits inside the same open-and-save that the prompt
       does rather than beside it. */
    if (port > ZERO) {
        int errorCode = serveWire(port);

        if (errorCode != SUCCESS_CODE)
            showError(errorCode);
    }
    /* A served session never reaches the prompt. */
    while (port == ZERO) {
        printf("\ndb> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;    /* EOF / Ctrl+Z */

        char* nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        else {                                          /* line too long: drain it */
            int c;
            while ((c = getchar()) != '\n' && c != EOF);
            fprintf(stderr, "statement too long, ignored\n");
            continue;
        }

        if (line[ZERO] == '\0') continue;

        CommandResult command = runDotCommand(line);

        if (command == QUIT)    break;
        if (command == HANDLED) continue;

        int errorCode = ProcessStatement(line, &results);

        errorCode == SUCCESS_CODE ? printResultSet(&results) : showError(errorCode);
    }

    /* Uncommitted work does not survive the session that wrote it. */
    if (inTransaction() && sessionPath[ZERO] != '\0')
        rollbackTransaction();

    if (sessionPath[ZERO] != '\0') {
        /* Each statement is already durable; this folds the log back into the
           file so what is left behind is one file rather than two. */
        int errorCode = usingLog ? closeDatabase() : saveDatabase(sessionPath);

        if (errorCode == SUCCESS_CODE)
            printf("saved to %s\n", sessionPath);
        else
            showError(errorCode);
    }

    freeResultSet(&results);
    FreeEngine();
    return ZERO;
}
