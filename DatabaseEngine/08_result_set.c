#include "sql_common.h"

/*
 * Output goes through a buffer of our own, formatted by hand.
 *
 * printf was costing 5.2 microseconds a row - fifty times what reading the row
 * out of its page costs - because a row is three or four format calls and the C
 * runtime's printf parses its format string, walks a varargs list and locks the
 * stream every time. A result set of any size was spending almost all of its
 * time printing.
 *
 * Everything here appends bytes it already knows the length of, and the buffer
 * reaches stdout in 64 KB writes.
 */
#define OUT_BUFFER 65536

static char   outBuffer[OUT_BUFFER];
static size_t outUsed;

static void flushOut(void)
{
    if (outUsed > ZERO) {
        fwrite(outBuffer, ONE, outUsed, stdout);
        outUsed = ZERO;
    }
}

static void emit(const char* text, size_t length)
{
    if (length > OUT_BUFFER - outUsed)
        flushOut();

    if (length > OUT_BUFFER) {              /* one value larger than the buffer */
        fwrite(text, ONE, length, stdout);
        return;
    }

    memcpy(outBuffer + outUsed, text, length);
    outUsed += length;
}

/* Decimal by hand: no format string to parse, no varargs to walk. */
static void emitInt(int value)
{
    char         digits[12];
    char         text[12];
    int          n = ZERO;
    /* negated as unsigned, so INT_MIN converts without overflowing */
    unsigned int rest = value < ZERO ? 0U - (unsigned int)value : (unsigned int)value;

    do {
        digits[n++] = (char)('0' + rest % 10u);
        rest /= 10u;
    } while (rest != ZERO);

    if (value < ZERO)
        digits[n++] = '-';

    for (int i = ZERO; i < n; i++)          /* the digits came out backwards */
        text[i] = digits[n - ONE - i];

    emit(text, (size_t)n);
}

/* The one place a format string is still worth it: getting a double to read
   back the way it was written is not something to hand-roll. */
static void emitFloat(double number)
{
    char text[32];
    int  length = snprintf(text, sizeof text, "%g", number);

    emit(text, (size_t)length);
}

static void printValue(const Value* v)
{
    if (v->isNull)
        emit("NULL", 4);
    else if (v->type == TYPE_INT)
        emitInt(v->intValue);
    else if (v->type == TYPE_FLOAT)
        emitFloat(v->floatValue);
    else if (v->type == TYPE_DATE) {
        char text[DATE_TEXT_LEN];

        dateToText(v->intValue, text);
        emit(text, 10);
    }
    else
        emit(v->text, (size_t)v->textLength);   /* length, not a terminator */
}

/*
 * Prints a result set as a plain column-separated table.
 */
void printResultSet(const ResultSet* results)
{
    /* A statement that only reports what it did is one short line, and one
       printf beats setting up a buffered write for it: measured, the buffer
       cost 0.9 microseconds a statement, which is most of what an INSERT
       costs. The buffer is for rows, where it pays for itself many times. */
    if (results->ncols == ZERO) {
        if (results->message[ZERO] != '\0')
            printf("%s\n", results->message);

        return;
    }

    for (int c = ZERO; c < results->ncols; c++) {
        if (c)
            emit(" | ", 3);
        emit(results->headers[c], strlen(results->headers[c]));
    }
    emit("\n", ONE);

    for (int r = ZERO; r < results->nrows; r++) {
        const Row* row = &results->rows[r];

        for (int c = ZERO; c < row->ncols; c++) {
            if (c)
                emit(" | ", 3);
            printValue(&row->values[c]);
        }
        emit("\n", ONE);
    }

    char tail[32];
    int  length = snprintf(tail, sizeof tail, "(%d row%s)\n",
                           results->nrows, results->nrows == ONE ? "" : "s");

    emit(tail, (size_t)length);
    flushOut();
}

/*
 * Grows the row array to hold at least this many rows. Doubling from a small
 * start means a query returning a handful of rows never allocates megabytes,
 * and one returning millions still gets there in a couple of dozen steps.
 */
int resultReserve(ResultSet* results, int rows)
{
    if (rows <= results->capacity)
        return SUCCESS_CODE;

    int grown = results->capacity ? results->capacity : INITIAL_RESULT_ROWS;
    while (grown < rows)
        grown *= TWO;

    Row* moved = (Row*)realloc(results->rows, (size_t)grown * sizeof(Row));
    if (moved == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    results->rows     = moved;
    results->capacity = grown;
    return SUCCESS_CODE;
}

void freeResultSet(ResultSet* results)
{
    free(results->rows);
    results->rows     = NULL;
    results->nrows    = ZERO;
    results->capacity = ZERO;
}
