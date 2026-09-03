/*
 * winnt.h has an enumerator called TokenType, and so does this engine. Renaming
 * theirs on the way in is the smallest fix that lets one file hold both; the
 * same collision is why 12_persist.c declares MoveFileExA by hand rather than
 * including <windows.h>. Nothing here uses the Windows one.
 */
#ifdef _WIN32
#define TokenType WindowsTokenType
#include <winsock2.h>
#include <ws2tcpip.h>
#undef TokenType
#else
/* Berkeley sockets under the names winsock gave them. The difference is
   almost entirely spelling: a socket is a plain descriptor, closing one is
   close(), and there is no library to start up or shut down. */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
/* struct timeval, for the receive timeout. winsock2.h supplies its own; on
   POSIX the socket headers do not declare it. */
#include <sys/time.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket    close
#define WSAStartup(v, d) (ZERO)
#define WSACleanup()     ((void)ZERO)
#define MAKEWORD(a, b)   (ZERO)
typedef int WSADATA;
#endif

#include "sql_common.h"
#include <ctype.h>
#include <time.h>
#include <errno.h>

/*
 * The PostgreSQL frontend/backend protocol, version 3, simple-query subset -
 * enough that the official psql connects to this engine and gets rows back.
 *
 * Every message after the startup packet is a type byte, an int32 length that
 * counts itself, and a body. Everything is big-endian, and every value is sent
 * in text format (format code 0), so there is no binary encoding to write: the
 * bytes a row prints are the bytes that go on the wire.
 *
 *   client -> SSLRequest        we answer 'N', declining, and it retries plain
 *   client -> StartupMessage    parameters, which we read and ignore
 *   server -> AuthenticationOk, ParameterStatus*, BackendKeyData, ReadyForQuery
 *   client -> Query 'Q'         one or more statements separated by semicolons
 *   server -> RowDescription 'T', DataRow 'D'*, CommandComplete 'C'
 *             or ErrorResponse 'E', then ReadyForQuery 'Z'
 *   client -> Terminate 'X'
 *
 * What this deliberately is not: psql's backslash commands are SQL against
 * pg_catalog, with joins and casts this engine does not have, so \d and its
 * relatives will not work. Typed SQL does.
 *
 * Many connections, one statement at a time. Everything a connection owns is
 * in a Session; the engine underneath is not - one catalog, one buffer pool,
 * one log - so statements are run one after another rather than at once.
 */

#define PROTOCOL_V3     196608u             /* 3.0, as the startup packet sends it */
#define SSL_REQUEST     80877103u
#define GSSENC_REQUEST  80877104u
#define CANCEL_REQUEST  80877102u

/* Postgres type OIDs, so psql knows what it is being handed. */
#define OID_INT4   23
#define OID_TEXT   25
#define OID_FLOAT8 701
#define OID_DATE   1082

#define WIRE_BUFFER 65536

#define MAX_PREPARED 8
#define MAX_PARAMS   32

typedef struct {
    int  used;
    char name[NAME_LEN];
    char sql[LINE_LEN];
    int  params;                            /* what Parse said it takes */
    int  types[MAX_PARAMS];                 /* parameter OIDs, 0 when unsaid */
} Prepared;

/*
 * One portal per session. A second one within a session would need its own
 * result set, and nothing this engine can do with two at once is worth the
 * copy.
 */
typedef struct {
    int       used;
    char      name[NAME_LEN];
    char      sql[LINE_LEN];                /* parameters already filled in */
    int       executed;
    int       sent;                         /* rows already handed over */
    ResultSet result;
} Portal;

/*
 * Concurrent connections.
 *
 * A connection gets a thread, and `self` is that thread's own Session - so
 * everything a connection owns is reached without a lock, because no other
 * thread can see it.
 *
 * The engine underneath is shared, and the engine lock is what makes that
 * safe: a SELECT takes it shared and any number run together, anything that
 * writes takes it exclusive and runs alone. That is the whole model. It is
 * coarse on purpose - one lock, held for one statement - and it is what "read
 * concurrency" means here.
 *
 * ponytail: a reader-writer lock over the whole engine rather than per-page
 * latches. Writers serialise against everything, so a write-heavy load is no
 * faster than it was; the upgrade is finer-grained locking in the pool and the
 * catalog, and it is a great deal of work for a workload nobody has yet.
 */
#define MAX_SESSIONS 16

/* A transaction holds the engine's write lock across statements, so a client
   that opens one and then stops talking would block every writer behind it.
   While a transaction is open the socket is given this timeout: the thread
   wakes on its own, rolls back, and lets go. Long enough that a real
   transaction is never cut short. */
#define IDLE_IN_TRANSACTION_SECONDS 30

typedef struct Session {
    SOCKET        socket;
    unsigned char out[WIRE_BUFFER];
    size_t        outUsed;
    Prepared      prepared[MAX_PREPARED];
    Portal        portal;
    /* Everything after an error is skipped until Sync, which is what the
       protocol says and what clients expect: they send a whole batch and then
       look. */
    int           ignoringUntilSync;
} Session;

/* This thread's session. Nothing else can reach it, which is why none of the
   code below takes a lock to touch it. */
static THREAD_LOCAL Session* self;

/* The count of live connections lives with the lock that guards it, in
   18_thread.c - see sessionAcquire. */

/* ---------- bytes on the wire ---------- */

static int flushWire(void)
{
    size_t at = ZERO;

    while (at < self->outUsed) {
        int sent = send(self->socket, (const char*)self->out + at,
                        (int)(self->outUsed - at), ZERO);

        if (sent <= ZERO) {
            self->outUsed = ZERO;
            return ERROR_IO_WRITE;
        }
        at += (size_t)sent;
    }

    self->outUsed = ZERO;
    return SUCCESS_CODE;
}

static int putBytes(const void* bytes, size_t length)
{
    if (length > WIRE_BUFFER) {                 /* one value larger than the buffer */
        int errorCode = flushWire();

        if (errorCode != SUCCESS_CODE)
            return errorCode;

        size_t at = ZERO;

        while (at < length) {
            int sent = send(self->socket, (const char*)bytes + at, (int)(length - at), ZERO);

            if (sent <= ZERO)
                return ERROR_IO_WRITE;
            at += (size_t)sent;
        }
        return SUCCESS_CODE;
    }

    if (self->outUsed + length > WIRE_BUFFER) {
        int errorCode = flushWire();

        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    memcpy(self->out + self->outUsed, bytes, length);
    self->outUsed += length;
    return SUCCESS_CODE;
}

/* Big-endian, unlike the file format: this is someone else's protocol. */
static int putI32(int value)
{
    unsigned char at[4];
    unsigned int  bits = (unsigned int)value;

    at[ZERO] = (unsigned char)((bits >> 24) & 0xFFu);
    at[1]    = (unsigned char)((bits >> 16) & 0xFFu);
    at[2]    = (unsigned char)((bits >> 8) & 0xFFu);
    at[3]    = (unsigned char)(bits & 0xFFu);

    return putBytes(at, 4);
}

static unsigned int readU32(const unsigned char* at)
{
    return ((unsigned int)at[ZERO] << 24) | ((unsigned int)at[1] << 16)
         | ((unsigned int)at[2] << 8) | (unsigned int)at[3];
}

/*
 * Whether the last recv gave up on time rather than on the connection. Only a
 * socket carrying an open transaction has a deadline at all, so this is the
 * difference between "the client is thinking" and "the client is holding the
 * write lock and has stopped talking".
 */
static int receiveTimedOut(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/*
 * Reads exactly this many bytes, or reports that the client went away. recv
 * returns what has arrived rather than what was asked for, so a message that
 * spans two packets has to be assembled here.
 */
static int readExactly(unsigned char* into, size_t length)
{
    size_t at = ZERO;

    while (at < length) {
        int got = recv(self->socket, (char*)into + at, (int)(length - at), ZERO);

        if (got <= ZERO)
            return receiveTimedOut() ? ERROR_IO_TIMED_OUT : ERROR_IO_CANNOT_OPEN;
        at += (size_t)got;
    }

    return SUCCESS_CODE;
}

/* ---------- messages ---------- */

/*
 * A message is a type byte, a length that counts itself, and a body - so the
 * length has to be known before the body is written. Every message here is
 * built into one buffer first for that reason.
 */
static unsigned char message[WIRE_BUFFER];
static size_t        messageUsed;

static void startMessage(void)
{
    messageUsed = ZERO;
}

static void addBytes(const void* bytes, size_t length)
{
    if (messageUsed + length > WIRE_BUFFER)     /* a header this big is a bug */
        return;

    memcpy(message + messageUsed, bytes, length);
    messageUsed += length;
}

static void addI32(int value)
{
    unsigned char at[4];
    unsigned int  bits = (unsigned int)value;

    at[ZERO] = (unsigned char)((bits >> 24) & 0xFFu);
    at[1]    = (unsigned char)((bits >> 16) & 0xFFu);
    at[2]    = (unsigned char)((bits >> 8) & 0xFFu);
    at[3]    = (unsigned char)(bits & 0xFFu);
    addBytes(at, 4);
}

static void addI16(int value)
{
    unsigned char at[TWO];

    at[ZERO] = (unsigned char)(((unsigned int)value >> 8) & 0xFFu);
    at[1]    = (unsigned char)((unsigned int)value & 0xFFu);
    addBytes(at, TWO);
}

static void addCString(const char* text)
{
    addBytes(text, strlen(text) + ONE);
}

static int sendMessage(char type)
{
    int errorCode = putBytes(&type, ONE);

    if (errorCode == SUCCESS_CODE)
        errorCode = putI32((int)messageUsed + 4);   /* the length counts itself */
    if (errorCode == SUCCESS_CODE)
        errorCode = putBytes(message, messageUsed);

    return errorCode;
}

/*
 * 'Z', which every exchange ends with. The status byte is what puts psql's
 * prompt into its "you are in a transaction" form, so it is read from the
 * engine rather than assumed.
 */
static int sendReadyForQuery(void)
{
    startMessage();
    addBytes(inTransaction() ? "T" : "I", ONE);

    int errorCode = sendMessage('Z');

    return errorCode == SUCCESS_CODE ? flushWire() : errorCode;
}

static int sendError(int code)
{
    startMessage();
    addBytes("S", ONE);  addCString("ERROR");
    addBytes("V", ONE);  addCString("ERROR");
    /* 42000 is "syntax error or access rule violation", which is the honest
       answer for most of what this engine refuses. */
    addBytes("C", ONE);  addCString("42000");
    addBytes("M", ONE);  addCString(errorCodeToString(code));
    addBytes("\0", ONE);                        /* end of the field list */

    return sendMessage('E');
}

/* ---------- results ---------- */

static int typeOid(ColType type)
{
    switch (type) {
    case TYPE_INT:   return OID_INT4;
    case TYPE_FLOAT: return OID_FLOAT8;
    case TYPE_DATE:  return OID_DATE;
    default:         return OID_TEXT;
    }
}

/*
 * A value as psql will read it. Text goes straight from the page bytes, so
 * only the scalars are rendered here - which is why this is not 08_result_set's
 * printValue: that one appends into its own output buffer through a hand-rolled
 * integer path that exists to make large result sets cheap to print.
 */
static int renderScalar(const Value* value, char* out, size_t size)
{
    if (value->type == TYPE_DATE) {
        dateToText(value->intValue, out);
        return 10;
    }

    if (value->type == TYPE_FLOAT)
        return snprintf(out, size, "%g", value->floatValue);

    return snprintf(out, size, "%d", value->intValue);
}

/*
 * 'T': one field description per column.
 */
static int sendRowDescription(const ResultSet* results)
{
    startMessage();
    addI16(results->ncols);

    for (int c = ZERO; c < results->ncols; c++) {
        /* From the result's own column types rather than from its first row:
           a query that matched nothing still has a shape, and a client asking
           what it returns is usually asking before there is a row to look at. */
        int oid = typeOid(results->types[c]);

        addCString(results->headers[c]);
        addI32(ZERO);                           /* not a column of a real table */
        addI16(ZERO);
        addI32(oid);
        addI16(oid == OID_INT4 ? 4 : oid == OID_FLOAT8 ? 8 : -1);
        addI32(-1);                             /* no type modifier */
        addI16(ZERO);                           /* text format */
    }

    return sendMessage('T');
}

static int sendDataRow(const Row* row, int ncols)
{
    char scalar[64];

    startMessage();
    addI16(ncols);

    for (int c = ZERO; c < ncols; c++) {
        const Value* value = &row->values[c];

        if (value->isNull) {
            addI32(-1);                         /* NULL is a length of -1 */
            continue;
        }

        if (value->type == TYPE_TEXT) {
            addI32(value->textLength);
            addBytes(value->text, (size_t)value->textLength);
            continue;
        }

        int length = renderScalar(value, scalar, sizeof scalar);

        addI32(length);
        addBytes(scalar, (size_t)length);
    }

    return sendMessage('D');
}

/*
 * The tag psql prints when a statement finishes. libpq parses the last field of
 * an INSERT tag as the row count, so that one keeps its "INSERT <oid> <rows>"
 * shape; the rest are the verb, with a count where a count means something.
 */
/* The next word, uppercased, and where it left off. */
static const char* takeWord(const char* sql, char* out)
{
    int n = ZERO;

    while (isspace((unsigned char)*sql))        /* a statement after a semicolon
                                                   starts with the space that
                                                   followed it */
        sql++;

    while (*sql != '\0' && !isspace((unsigned char)*sql) && n < NAME_LEN - ONE)
        out[n++] = (char)toupper((unsigned char)*sql++);

    out[n] = '\0';
    return sql;
}

static void commandTag(const char* sql, const ResultSet* results, char* out, size_t size)
{
    char verb[NAME_LEN];
    char object[NAME_LEN];

    const char* rest = takeWord(sql, verb);

    if (results->ncols > ZERO) {
        snprintf(out, size, "SELECT %d", results->nrows);
        return;
    }

    /* libpq reads the last field of an INSERT tag as the row count, so that
       one keeps its shape whatever else changes. */
    if (strcmp(verb, "INSERT") == ZERO) {
        snprintf(out, size, "INSERT 0 %d", results->rowsAffected);
        return;
    }

    if (strcmp(verb, "UPDATE") == ZERO || strcmp(verb, "DELETE") == ZERO) {
        snprintf(out, size, "%s %d", verb, results->rowsAffected);
        return;
    }

    /* CREATE and DROP are named after what they act on, the way Postgres
       reports them - psql prints this tag verbatim. */
    if (strcmp(verb, "CREATE") == ZERO || strcmp(verb, "DROP") == ZERO) {
        takeWord(rest, object);

        if (object[ZERO] != '\0') {
            snprintf(out, size, "%s %s", verb, object);
            return;
        }
    }

    snprintf(out, size, "%s", verb[ZERO] != '\0' ? verb : "OK");
}

static int sendResult(const char* sql, const ResultSet* results)
{
    int errorCode = SUCCESS_CODE;

    if (results->ncols > ZERO) {
        errorCode = sendRowDescription(results);

        for (int r = ZERO; r < results->nrows && errorCode == SUCCESS_CODE; r++)
            errorCode = sendDataRow(&results->rows[r], results->ncols);
    }

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    char tag[NAME_LEN + 32];

    commandTag(sql, results, tag, sizeof tag);

    startMessage();
    addCString(tag);
    return sendMessage('C');
}

/* ---------- the query itself ---------- */

/*
 * One Query message may carry several statements, and this engine runs one at a
 * time, so the string is cut on the semicolons that are actually separators.
 *
 * Quotes and comments are why this is not strchr: a semicolon inside 'a;b' is
 * data, and a comment has to be dropped here rather than left for the lexer -
 * the lexer stops at -- and would throw away the rest of a multi-line query
 * with it.
 *
 * Returns where the next statement starts, or NULL at the end. A statement too
 * long for the buffer sets *overflow rather than being quietly shortened: a
 * truncated statement can still parse, and "select ... where a and b" with the
 * b cut off is a wrong answer rather than an error - which is the worst thing a
 * database can do with a query.
 */
static const char* nextStatement(const char* sql, char* out, size_t size,
                                 int* overflow)
{
    size_t n      = ZERO;
    int    inText = ZERO;
    int    any    = ZERO;

    *overflow = ZERO;

    while (*sql != '\0') {
        if (inText) {
            if (*sql == '\'') {
                /* '' is an escaped quote and stays inside the string */
                if (sql[1] == '\'') {
                    if (n + TWO >= size) {
                        *overflow = ONE;
                        return NULL;
                    }
                    out[n++] = *sql;
                    out[n++] = sql[1];
                    sql += TWO;
                    continue;
                }
                inText = ZERO;
            }

            if (n + ONE >= size) {
                *overflow = ONE;
                return NULL;
            }

            out[n++] = *sql;
            sql++;
            continue;
        }

        if (*sql == '\'') {
            inText = ONE;
            any    = ONE;

            if (n + ONE >= size) {
                *overflow = ONE;
                return NULL;
            }

            out[n++] = *sql;
            sql++;
            continue;
        }

        if (*sql == '-' && sql[1] == '-') {     /* comment, to end of line */
            while (*sql != '\0' && *sql != '\n')
                sql++;
            continue;
        }

        if (*sql == ';') {
            sql++;
            out[n] = '\0';
            return any ? sql : nextStatement(sql, out, size, overflow);
        }

        if (!isspace((unsigned char)*sql))
            any = ONE;

        if (n + ONE >= size) {
            *overflow = ONE;
            return NULL;
        }

        out[n++] = *sql;
        sql++;
    }

    out[n] = '\0';
    return any ? sql : NULL;                    /* trailing whitespace is not a statement */
}

static int runQuery(const char* sql, ResultSet* results)
{
    char        statement[LINE_LEN];
    const char* at       = sql;
    int         overflow = ZERO;

    while ((at = nextStatement(at, statement, sizeof statement,
                               &overflow)) != NULL) {
        int errorCode = ProcessStatement(statement, results);

        if (errorCode != SUCCESS_CODE) {
            /* Postgres abandons the rest of the message on an error, and psql
               expects exactly one ReadyForQuery after it. */
            return sendError(errorCode);
        }

        errorCode = sendResult(statement, results);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        if (*at == '\0')
            break;
    }

    return overflow ? sendError(ERROR_TOO_MANY_TOKENS) : SUCCESS_CODE;
}


/* ---------- the extended query protocol ---------- */

/*
 * Parse / Bind / Describe / Execute / Sync, which is how every driver that is
 * not psql talks: JDBC, and psycopg2 whenever it binds server-side. The simple
 * Query path above is unchanged and still does the work.
 *
 * Parameters are substituted into the statement text at Bind rather than being
 * passed down as values. This engine plans nothing and caches nothing, so a
 * prepared statement is only a string with holes in it, and filling them in is
 * the whole of what "binding" can mean here. It also means one place decides
 * how a parameter is spelled, which is where the type has to be guessed.
 */

/* The types these use are declared with the session, above. */

/* ---------- reading a message body ---------- */

typedef struct {
    const unsigned char* data;
    size_t               size;
    size_t               at;
} Body;

static int bodyString(Body* body, char* out, size_t size)
{
    size_t n = ZERO;

    while (body->at < body->size && body->data[body->at] != ZERO) {
        if (n + ONE < size)
            out[n++] = (char)body->data[body->at];
        body->at++;
    }

    if (body->at >= body->size)                 /* no terminator: truncated */
        return ERROR_IO_BAD_FORMAT;

    body->at++;
    out[n] = '\0';
    return SUCCESS_CODE;
}

static int bodyI16(Body* body, int* out)
{
    if (body->at + TWO > body->size)
        return ERROR_IO_BAD_FORMAT;

    *out = (int)((body->data[body->at] << 8) | body->data[body->at + ONE]);
    body->at += TWO;
    return SUCCESS_CODE;
}

static int bodyI32(Body* body, int* out)
{
    if (body->at + 4 > body->size)
        return ERROR_IO_BAD_FORMAT;

    *out = (int)readU32(body->data + body->at);
    body->at += 4;
    return SUCCESS_CODE;
}

/* ---------- prepared statements ---------- */

static Prepared* findPrepared(const char* name)
{
    for (int i = ZERO; i < MAX_PREPARED; i++)
        if (self->prepared[i].used && strcmp(self->prepared[i].name, name) == ZERO)
            return &self->prepared[i];
    return NULL;
}

static Prepared* claimPrepared(const char* name)
{
    Prepared* existing = findPrepared(name);

    /* An unnamed statement is replaced without ceremony; that is what unnamed
       means, and every driver reuses it. */
    if (existing != NULL)
        return existing;

    for (int i = ZERO; i < MAX_PREPARED; i++)
        if (!self->prepared[i].used) {
            self->prepared[i].used = ONE;
            snprintf(self->prepared[i].name, NAME_LEN, "%s", name);
            return &self->prepared[i];
        }

    return NULL;
}

/*
 * How a bound parameter is written into the statement.
 *
 * The type is whatever Parse declared; when it declared nothing - which is
 * common - the text is read to see whether it looks like a number. Getting
 * this wrong is not silent: an unquoted word is not a column and a quoted
 * number is not an int, and either way the statement is refused rather than
 * answered oddly.
 */
static int parameterIsNumeric(int oid, const char* text, int length)
{
    if (oid == OID_INT4 || oid == OID_FLOAT8 || oid == 20 || oid == 21
        || oid == 700 || oid == 1700)
        return ONE;

    if (oid != ZERO || length == ZERO)          /* a stated type that is not a number */
        return ZERO;

    int at    = (text[ZERO] == '-' || text[ZERO] == '+') ? ONE : ZERO;
    int seen  = ZERO;
    int dots  = ZERO;

    if (at == length)
        return ZERO;

    for (; at < length; at++) {
        if (text[at] >= '0' && text[at] <= '9') {
            seen = ONE;
            continue;
        }
        if (text[at] == '.' && !dots) {
            dots = ONE;
            continue;
        }
        return ZERO;
    }

    return seen;
}

/*
 * Writes the statement with $1, $2 ... replaced by what Bind sent.
 *
 * A dollar inside a string literal is data, so the scan tracks quotes the same
 * way the statement splitter does. A parameter with no value is NULL, and a
 * text one is quoted with its own quotes doubled - which is the only escaping
 * this dialect has.
 *
 * `spelled` says the values are already written as SQL literals, quotes and
 * all, which only the shape probe does. It is a separate argument rather than
 * something inferred from the bytes: a real parameter beginning with a quote
 * is a string that happens to start with one, and deciding by looking would
 * hand the client a way to write its own SQL.
 */
static int substituteParameters(const char* sql, const Prepared* statement,
                                const unsigned char* values[], const int lengths[],
                                int count, int spelled, char* out, size_t size)
{
    size_t n      = ZERO;
    int    inText = ZERO;

    while (*sql != '\0') {
        if (inText) {
            if (*sql == '\'')
                inText = ZERO;
        }
        else if (*sql == '\'') {
            inText = ONE;
        }
        else if (*sql == '$' && sql[1] >= '1' && sql[1] <= '9') {
            int at = ZERO;

            sql++;
            while (*sql >= '0' && *sql <= '9') {
                /* Stop counting well before an int would wrap. "$99999999999"
                   is not a parameter anyone bound, and letting it overflow
                   would turn the index below into a negative one. */
                if (at > MAX_PARAMS)
                    return ERROR_SYNTAX_EXPECTED_VALUE;

                at = at * 10 + (*sql - '0');
                sql++;
            }

            if (at < ONE || at > count)         /* asked for what Bind never sent */
                return ERROR_SYNTAX_EXPECTED_VALUE;

            const unsigned char* value  = values[at - ONE];
            int                  length = lengths[at - ONE];
            int                  oid    = at <= MAX_PARAMS ? statement->types[at - ONE]
                                                           : ZERO;

            if (value == NULL) {                /* a NULL parameter */
                if (n + 4 >= size)
                    return ERROR_TOO_MANY_TOKENS;

                memcpy(out + n, "NULL", 4);
                n += 4;
                continue;
            }

            if (spelled || parameterIsNumeric(oid, (const char*)value, length)) {
                if (n + (size_t)length >= size)
                    return ERROR_TOO_MANY_TOKENS;

                memcpy(out + n, value, (size_t)length);
                n += (size_t)length;
                continue;
            }

            if (n + ONE >= size)
                return ERROR_TOO_MANY_TOKENS;
            out[n++] = '\'';

            for (int i = ZERO; i < length; i++) {
                if (n + TWO >= size)
                    return ERROR_TOO_MANY_TOKENS;

                if (value[i] == '\'')          /* '' is one literal quote */
                    out[n++] = '\'';
                out[n++] = (char)value[i];
            }

            if (n + ONE >= size)
                return ERROR_TOO_MANY_TOKENS;
            out[n++] = '\'';
            continue;
        }

        if (n + ONE >= size)
            return ERROR_TOO_MANY_TOKENS;

        out[n++] = *sql++;
    }

    out[n] = '\0';
    return SUCCESS_CODE;
}

/*
 * What to put where a parameter goes when the statement is being run only to
 * find out what columns it returns.
 *
 * NULL will not do: "id >= NULL" is refused by the parser on purpose, because
 * a human who writes it meant IS NULL. So the probe needs a real value, and
 * which one depends on the column the parameter is being compared with - which
 * is exactly what nobody has said yet. The declared type settles it when there
 * is one, and when there is not the candidates are tried in turn until the
 * statement parses.
 */
static const char* probeLiteral(int oid, int attempt)
{
    if (attempt == ZERO) {
        switch (oid) {
        case OID_TEXT:   return "''";
        case OID_DATE:   return "'2000-01-01'";
        case OID_FLOAT8: return "0.0";
        default:         return "0";
        }
    }

    return attempt == ONE ? "''" : "'2000-01-01'";
}

#define PROBE_ATTEMPTS 3

/* ---------- the messages ---------- */

static int sendSimple(char type)
{
    startMessage();
    return sendMessage(type);
}

static int failExtended(int code)
{
    self->ignoringUntilSync = ONE;
    return sendError(code);
}

static int handleParse(Body* body)
{
    char name[NAME_LEN];
    char sql[LINE_LEN];
    int  count;

    if (bodyString(body, name, sizeof name) != SUCCESS_CODE
        || bodyString(body, sql, sizeof sql) != SUCCESS_CODE
        || bodyI16(body, &count) != SUCCESS_CODE || count < ZERO)
        return failExtended(ERROR_IO_BAD_FORMAT);

    Prepared* statement = claimPrepared(name);

    if (statement == NULL)
        return failExtended(ERROR_EXEC_OUT_OF_MEMORY);

    snprintf(statement->sql, sizeof statement->sql, "%s", sql);
    statement->params = count < MAX_PARAMS ? count : MAX_PARAMS;

    for (int i = ZERO; i < count; i++) {
        int oid = ZERO;

        if (bodyI32(body, &oid) != SUCCESS_CODE)
            return failExtended(ERROR_IO_BAD_FORMAT);

        if (i < MAX_PARAMS)
            statement->types[i] = oid;
    }

    return sendSimple('1');                     /* ParseComplete */
}

static int handleBind(Body* body)
{
    char name[NAME_LEN];
    char source[NAME_LEN];
    int  formats;

    if (bodyString(body, name, sizeof name) != SUCCESS_CODE
        || bodyString(body, source, sizeof source) != SUCCESS_CODE
        || bodyI16(body, &formats) != SUCCESS_CODE || formats < ZERO)
        return failExtended(ERROR_IO_BAD_FORMAT);

    for (int i = ZERO; i < formats; i++) {
        int code;

        if (bodyI16(body, &code) != SUCCESS_CODE)
            return failExtended(ERROR_IO_BAD_FORMAT);

        /* Binary parameters would have to be decoded per type, and every
           client can send text instead. Saying so beats reading them wrong. */
        if (code != ZERO)
            return failExtended(ERROR_SEMANTIC_TYPE_MISMATCH);
    }

    Prepared* statement = findPrepared(source);

    if (statement == NULL)
        return failExtended(ERROR_SEMANTIC_TABLE_NOT_FOUND);

    int count;

    if (bodyI16(body, &count) != SUCCESS_CODE || count < ZERO || count > MAX_PARAMS)
        return failExtended(ERROR_IO_BAD_FORMAT);

    const unsigned char* values[MAX_PARAMS];
    int                  lengths[MAX_PARAMS];

    for (int i = ZERO; i < count; i++) {
        int length;

        if (bodyI32(body, &length) != SUCCESS_CODE)
            return failExtended(ERROR_IO_BAD_FORMAT);

        if (length < ZERO) {                    /* NULL */
            values[i]  = NULL;
            lengths[i] = ZERO;
            continue;
        }

        if (body->at + (size_t)length > body->size)
            return failExtended(ERROR_IO_BAD_FORMAT);

        values[i]  = body->data + body->at;
        lengths[i] = length;
        body->at  += (size_t)length;
    }

    int errorCode = substituteParameters(statement->sql, statement, values,
                                         lengths, count, ZERO, self->portal.sql,
                                         sizeof self->portal.sql);
    if (errorCode != SUCCESS_CODE)
        return failExtended(errorCode);

    snprintf(self->portal.name, NAME_LEN, "%s", name);
    self->portal.used     = ONE;
    self->portal.executed = ZERO;
    self->portal.sent     = ZERO;

    return sendSimple('2');                     /* BindComplete */
}

/*
 * Runs the portal, once, and keeps the rows.
 *
 * Nothing here streams: the whole result is built and then handed out, because
 * that is how the executor works and a cursor over a live scan would be a
 * different engine. Execute with a row limit still stops where it was asked to
 * and reports the portal as suspended.
 */
static int runPortal(void)
{
    if (!self->portal.used)
        return ERROR_SEMANTIC_TABLE_NOT_FOUND;
    if (self->portal.executed)
        return SUCCESS_CODE;

    int errorCode = ProcessStatement(self->portal.sql, &self->portal.result);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    self->portal.executed = ONE;
    self->portal.sent     = ZERO;
    return SUCCESS_CODE;
}

static int handleDescribe(Body* body)
{
    char what[TWO] = { ZERO, ZERO };
    char name[NAME_LEN];

    if (body->at >= body->size)
        return failExtended(ERROR_IO_BAD_FORMAT);

    what[ZERO] = (char)body->data[body->at++];

    if (bodyString(body, name, sizeof name) != SUCCESS_CODE)
        return failExtended(ERROR_IO_BAD_FORMAT);

    if (what[ZERO] == 'S') {
        Prepared* statement = findPrepared(name);

        if (statement == NULL)
            return failExtended(ERROR_SEMANTIC_TABLE_NOT_FOUND);

        /* The parameters, with no types claimed for them: this engine works
           out what a literal is from the column it meets, so promising an OID
           here would be inventing one. */
        startMessage();
        addI16(statement->params);

        for (int i = ZERO; i < statement->params; i++)
            addI32(ZERO);

        int errorCode = sendMessage('t');       /* ParameterDescription */

        if (errorCode != SUCCESS_CODE)
            return errorCode;

        /*
         * The shape of a result is not known until the statement runs, and
         * this engine plans nothing it could ask instead. A SELECT changes
         * nothing, though, so it can be run here with its parameters left
         * NULL purely to see what columns come back - and the answer is
         * thrown away. Anything that would write is described as returning
         * nothing, which is what it does.
         *
         * Drivers need this. pg8000 describes the statement rather than the
         * portal, and answering NoData to a SELECT leaves it with nowhere to
         * put the rows that arrive a moment later.
         */
        char verb[NAME_LEN];

        takeWord(statement->sql, verb);

        if (strcmp(verb, "SELECT") != ZERO)
            return sendSimple('n');             /* NoData */

        static ResultSet shape;
        static char      probe[LINE_LEN];

        errorCode = ERROR_SYNTAX_EXPECTED_VALUE;

        for (int attempt = ZERO;
             attempt < PROBE_ATTEMPTS && errorCode != SUCCESS_CODE; attempt++) {
            const unsigned char* stand[MAX_PARAMS];
            int                  lengths[MAX_PARAMS];

            for (int i = ZERO; i < MAX_PARAMS; i++) {
                const char* text = probeLiteral(i < statement->params
                                                ? statement->types[i] : ZERO,
                                                attempt);

                /* written straight through, quotes and all */
                stand[i]   = (const unsigned char*)text;
                lengths[i] = (int)strlen(text);
            }

            /* Every slot, not the count Parse declared: a client may leave
               the types unsaid and still write $1, and the probe has a
               stand-in ready for all of them either way. */
            if (substituteParameters(statement->sql, statement, stand, lengths,
                                     MAX_PARAMS, ONE, probe, sizeof probe)
                != SUCCESS_CODE)
                return failExtended(ERROR_SYNTAX_EXPECTED_VALUE);

            errorCode = ProcessStatement(probe, &shape);
        }

        if (errorCode != SUCCESS_CODE)
            return failExtended(errorCode);

        if (shape.ncols == ZERO)
            return sendSimple('n');             /* NoData */

        return sendRowDescription(&shape);
    }

    int errorCode = runPortal();

    if (errorCode != SUCCESS_CODE)
        return failExtended(errorCode);

    if (self->portal.result.ncols == ZERO)
        return sendSimple('n');                 /* NoData */

    return sendRowDescription(&self->portal.result);
}

static int handleExecute(Body* body)
{
    char name[NAME_LEN];
    int  limit;

    if (bodyString(body, name, sizeof name) != SUCCESS_CODE
        || bodyI32(body, &limit) != SUCCESS_CODE)
        return failExtended(ERROR_IO_BAD_FORMAT);

    int errorCode = runPortal();

    if (errorCode != SUCCESS_CODE)
        return failExtended(errorCode);

    int last = self->portal.result.nrows;

    if (limit > ZERO && self->portal.sent + limit < last)
        last = self->portal.sent + limit;

    for (int r = self->portal.sent; r < last; r++) {
        errorCode = sendDataRow(&self->portal.result.rows[r], self->portal.result.ncols);

        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    self->portal.sent = last;

    /* More rows waiting means the portal is suspended rather than finished,
       and the client will ask again. */
    if (self->portal.sent < self->portal.result.nrows)
        return sendSimple('s');                 /* PortalSuspended */

    char tag[NAME_LEN + 32];

    commandTag(self->portal.sql, &self->portal.result, tag, sizeof tag);

    startMessage();
    addCString(tag);
    return sendMessage('C');
}

static int handleClose(Body* body)
{
    char what[TWO] = { ZERO, ZERO };
    char name[NAME_LEN];

    if (body->at >= body->size)
        return failExtended(ERROR_IO_BAD_FORMAT);

    what[ZERO] = (char)body->data[body->at++];

    if (bodyString(body, name, sizeof name) != SUCCESS_CODE)
        return failExtended(ERROR_IO_BAD_FORMAT);

    if (what[ZERO] == 'S') {
        Prepared* statement = findPrepared(name);

        if (statement != NULL)
            statement->used = ZERO;
    }
    else if (self->portal.used && strcmp(self->portal.name, name) == ZERO) {
        self->portal.used     = ZERO;
        self->portal.executed = ZERO;
    }

    return sendSimple('3');                     /* CloseComplete */
}

/* ---------- connection ---------- */

/*
 * The startup exchange. psql tries SSL first unless told not to, and a server
 * that ignores the request rather than declining it leaves psql waiting for a
 * byte that never comes - which looks like a hang, not a refusal.
 */
static int handshake(void)
{
    unsigned char header[4];

    for (;;) {
        int errorCode = readExactly(header, 4);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        unsigned int length = readU32(header);

        if (length < 8 || length > WIRE_BUFFER)
            return ERROR_IO_BAD_FORMAT;

        static unsigned char body[WIRE_BUFFER];

        errorCode = readExactly(body, length - 4);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        unsigned int version = readU32(body);

        if (version == SSL_REQUEST || version == GSSENC_REQUEST) {
            char no = 'N';                      /* declined; it will retry plain */

            errorCode = putBytes(&no, ONE);
            if (errorCode == SUCCESS_CODE)
                errorCode = flushWire();
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            continue;
        }

        /* Cancellation would have to interrupt a statement already running,
           and statements do not overlap here - the request is refused rather
           than silently ignored. */
        if (version == CANCEL_REQUEST)
            return ERROR_IO_CANNOT_OPEN;

        if (version != PROTOCOL_V3)
            return ERROR_IO_VERSION;

        break;                                  /* the parameters are not needed */
    }

    startMessage();
    addI32(ZERO);                               /* AuthenticationOk: trust */
    int errorCode = sendMessage('R');

    /* What psql reads before it will talk: enough that it does not guess. */
    static const char* settings[] = {
        "server_version",               "9.6.0 (minisql)",
        "server_encoding",              "UTF8",
        "client_encoding",              "UTF8",
        "DateStyle",                    "ISO, MDY",
        "standard_conforming_strings",  "on",
        "integer_datetimes",            "on",
        "TimeZone",                     "UTC",
    };

    for (size_t i = ZERO; i < sizeof settings / sizeof settings[ZERO]
                          && errorCode == SUCCESS_CODE; i += TWO) {
        startMessage();
        addCString(settings[i]);
        addCString(settings[i + ONE]);
        errorCode = sendMessage('S');
    }

    if (errorCode == SUCCESS_CODE) {
        startMessage();
        addI32(1);                              /* a process id, and a key that */
        addI32(1);                              /* no cancel request will use */
        errorCode = sendMessage('K');
    }

    return errorCode == SUCCESS_CODE ? sendReadyForQuery() : errorCode;
}

/*
 * One session, until the client says Terminate or drops.
 */
/*
 * One message from one session. Returns SUCCESS_CODE while the connection is
 * still good; anything else means it is finished and should be closed.
 *
 * The read is blocking once the socket says it has something: a message can
 * span packets, and assembling it here is far simpler than keeping a partial
 * message per session. A client that sends half a message and stalls therefore
 * stalls the server, which is the honest cost of not writing a state machine.
 */
static int serveMessage(ResultSet* results)
{
    unsigned char header[5];

    /* Passed through rather than flattened: a receive timeout means this
       connection is idle in a transaction and holding the write lock,
       which the caller reports and acts on differently from a client that
       simply went away. */
    int errorCode = readExactly(header, 5);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    char         type   = (char)header[ZERO];
    unsigned int length = readU32(header + ONE);

    if (length < 4 || length > WIRE_BUFFER)
        return ERROR_IO_BAD_FORMAT;

    static unsigned char body[WIRE_BUFFER];

    body[ZERO] = ZERO;
    if (length > 4 && readExactly(body, length - 4) != SUCCESS_CODE)
        return ERROR_IO_CANNOT_OPEN;

    body[length - 4] = ZERO;                /* the string is NUL-terminated */

    if (type == 'X')                        /* Terminate */
        return ERROR_IO_CANNOT_OPEN;

    Body reader = { body, length - 4, ZERO };

    /* After an error the protocol says to skip to the next Sync, and
       clients rely on it: they send a whole batch and then look. */
    if (self->ignoringUntilSync && type != 'S')
        return SUCCESS_CODE;

    errorCode = SUCCESS_CODE;

    switch (type) {
    case 'Q':
        /*
         * A simple query destroys the unnamed portal, which the protocol
         * says and this engine needs: a result's text lives in the
         * statement arena, and running anything else winds that arena
         * back. Rows kept from an earlier Execute would be pointing at
         * memory the next statement has already reused.
         */
        self->portal.used     = ZERO;
        self->portal.executed = ZERO;

        errorCode = runQuery((const char*)body, results);
        if (errorCode == SUCCESS_CODE)
            errorCode = sendReadyForQuery();
        break;

    case 'P': errorCode = handleParse(&reader);    break;
    case 'B': errorCode = handleBind(&reader);     break;
    case 'D': errorCode = handleDescribe(&reader); break;
    case 'E': errorCode = handleExecute(&reader);  break;
    case 'C': errorCode = handleClose(&reader);    break;

    case 'H':                               /* Flush */
        errorCode = flushWire();
        break;

    case 'S':                               /* Sync ends the batch */
        self->ignoringUntilSync = ZERO;
        errorCode = sendReadyForQuery();
        break;

    default:
        errorCode = failExtended(ERROR_SYNTAX_INVALID_STATEMENT);
        break;
    }

    return errorCode;
}

/*
 * Listens until killed, one thread per client.
 */
static void closeSession(void)
{
    /* A session that ends mid-transaction has not committed it, and no other
       connection may inherit it - not the rows, and not the status byte that
       would tell the next client it is inside one. abandonTransaction ends it
       whether or not the database can roll back. This happens under the write
       lock this thread is still holding, and only then is the lock given
       back. */
    if (engineInTransaction()) {
        abandonTransaction();
        engineReleaseTransaction();
    }

    closesocket(self->socket);
    freeResultSet(&self->portal.result);
    free(self);
    self = NULL;

    /* Everything the executor and the arena allocated for this thread is this
       thread's own, so it has to be released here rather than at exit -
       nothing else will ever see it again. resetTextArena releases the chunks
       as well as winding back, which is all this needs. */
    freeExecutor();
    resetTextArena();

    sessionRelease();

    printf("client disconnected\n");
    fflush(stdout);
}

/*
 * A transaction holds the engine's write lock between statements, so a client
 * that opens one and then goes quiet would block every writer behind it. The
 * socket gets a receive timeout for exactly as long as that is true.
 */
static void setIdleTimeout(int seconds)
{
#ifdef _WIN32
    DWORD milliseconds = (DWORD)(seconds * 1000);
    setsockopt(self->socket, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&milliseconds, sizeof milliseconds);
#else
    struct timeval wait;

    wait.tv_sec  = seconds;
    wait.tv_usec = ZERO;
    setsockopt(self->socket, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&wait, sizeof wait);
#endif
}

static void* serveClient(void* raw)
{
    ResultSet results = { ZERO };

    self = (Session*)raw;

    printf("client connected\n");
    fflush(stdout);

    if (handshake() == SUCCESS_CODE)
        for (;;) {
            /* Only while a transaction is open: an idle connection that holds
               nothing is welcome to stay idle forever. */
            setIdleTimeout(engineInTransaction() ? IDLE_IN_TRANSACTION_SECONDS
                                                 : ZERO);

            int errorCode = serveMessage(&results);

            if (errorCode == ERROR_IO_TIMED_OUT) {
                printf("session idle in transaction, rolled back\n");
                fflush(stdout);
                break;
            }

            if (errorCode != SUCCESS_CODE)
                break;
        }

    freeResultSet(&results);
    closeSession();
    return NULL;
}

int serveWire(int port)
{
    WSADATA         winsock;
    struct sockaddr_in address;

    if (WSAStartup(MAKEWORD(2, 2), &winsock) != ZERO)
        return ERROR_IO_CANNOT_OPEN;

    /* From here on there is more than one thread, and the locks stop being
       no-ops. Nothing before this point needed them. */
    initThreads();

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (listener == INVALID_SOCKET) {
        WSACleanup();
        return ERROR_IO_CANNOT_OPEN;
    }

    /* so a restart does not have to wait out the previous socket's TIME_WAIT */
    int reuse = ONE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof reuse);

    memset(&address, ZERO, sizeof address);
    address.sin_family      = AF_INET;
    address.sin_port        = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);    /* this machine only */

    if (bind(listener, (struct sockaddr*)&address, sizeof address) != ZERO
        || listen(listener, MAX_SESSIONS) != ZERO) {
        closesocket(listener);
        WSACleanup();
        return ERROR_IO_CANNOT_OPEN;
    }

    printf("listening on 127.0.0.1:%d - psql -h 127.0.0.1 -p %d\n", port, port);
    fflush(stdout);

    for (;;) {
        SOCKET incoming = accept(listener, NULL, NULL);

        if (incoming == INVALID_SOCKET)
            break;

        /* calloc, so the session starts with no prepared statements, no
           portal, and nothing to skip to Sync - which is exactly what a new
           connection must look like. */
        Session* session = sessionAcquire(MAX_SESSIONS)
                         ? (Session*)calloc(ONE, sizeof(Session)) : NULL;

        if (session == NULL) {
            /* Out of slots or out of memory. Closing is the honest answer: the
               client sees a refused connection rather than a silence it has to
               time out. */
            closesocket(incoming);
            continue;
        }

        session->socket = incoming;

        if (threadStart(serveClient, session) != SUCCESS_CODE) {
            closesocket(incoming);
            free(session);
            sessionRelease();
        }
    }

    closesocket(listener);
    WSACleanup();
    freeThreads();
    return SUCCESS_CODE;
}
