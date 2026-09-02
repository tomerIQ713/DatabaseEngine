#include "sql_common.h"
#include <ctype.h>

/*
 * Keyword lookup is a jump table on the first four bytes rather than a walk of
 * a keyword table comparing strings.
 *
 * The four bytes are loaded into an integer with a fixed-size memcpy - which the
 * compiler turns into one load - and the switch over that integer compiles to a
 * jump table, so an unrecognised word is rejected in a single dispatch instead
 * of thirty-six calls to _stricmp.
 *
 * Two things this has to get right that a byte-for-byte compare got for free:
 *
 *   Case. ORing 0x20 lowercases an ASCII letter, and applied to the packed
 *   integer it folds all four bytes at once. PACK_4_LOWER applies exactly the
 *   same fold at compile time, so label and loaded word always agree.
 *
 *   Short keywords. "by" and "is" are shorter than the four bytes being read,
 *   so the word is staged through a zero-filled buffer rather than read past
 *   its end. A NUL pad folds to 0x20, which the macro reproduces for the label.
 *
 * The prefixes of all thirty-six keywords are distinct, so a case identifies one
 * keyword and the only remaining question is whether the rest of the word
 * matches - "selection" packs the same as "select" and is an identifier.
 */
#define KEYWORD_MAX   8                 /* "database", "distinct", "rollback" */

/* ORing 0x20 lowercases an ASCII letter, and a NUL pad folds to 0x20 with it,
   so a short keyword's label matches the zero-filled buffer the word is staged
   through. Applied to the packed integer it folds all four bytes at once. */
#define FOLD(c)                  ((c) | 0x20)
#define PACK_4_LOWER(a, b, c, d) PACK_4(FOLD(a), FOLD(b), FOLD(c), FOLD(d))

/* The switch has already matched four bytes; this settles the rest. The length
   test is what rejects a longer word that happens to start with a keyword, and
   the (n) <= 4 arm folds away at compile time for the short ones. */
#define KEYWORD(n, rest, token)                                               \
    return (length == (size_t)(n)                                             \
            && ((n) <= 4                                                      \
                || _strnicmp(word + 4, rest, (size_t)((n) - 4)) == ZERO))     \
         ? (token) : TOKEN_IDENTIFIER

/*
 * A word is a keyword if it matches the table, an identifier otherwise.
 */
static TokenType classifyWord(const char* word, size_t length)
{
    /* nothing in the table is longer than this, so junk leaves immediately */
    if (length > KEYWORD_MAX)
        return TOKEN_IDENTIFIER;

    char head[4] = { ZERO, ZERO, ZERO, ZERO };
    memcpy(head, word, length < 4 ? length : 4);

    uint32_t prefix;
    memcpy(&prefix, head, sizeof prefix);
    prefix |= 0x20202020u;              /* fold all four bytes in one go */

    switch (prefix) {
    case PACK_4_LOWER('s', 'e', 'l', 'e'):  KEYWORD(6, "ct",  TOKEN_KEYWORD_SELECT);
    case PACK_4_LOWER('f', 'r', 'o', 'm'):  KEYWORD(4, "",    TOKEN_KEYWORD_FROM);
    case PACK_4_LOWER('w', 'h', 'e', 'r'):  KEYWORD(5, "e",   TOKEN_KEYWORD_WHERE);
    case PACK_4_LOWER('g', 'r', 'o', 'u'):  KEYWORD(5, "p",   TOKEN_KEYWORD_GROUP);
    case PACK_4_LOWER('b', 'y', 0, 0):      KEYWORD(2, "",    TOKEN_KEYWORD_BY);
    case PACK_4_LOWER('n', 'u', 'l', 'l'):  KEYWORD(4, "",    TOKEN_KEYWORD_NULL);
    case PACK_4_LOWER('i', 's', 0, 0):      KEYWORD(2, "",    TOKEN_KEYWORD_IS);
    case PACK_4_LOWER('n', 'o', 't', 0):    KEYWORD(3, "",    TOKEN_KEYWORD_NOT);
    case PACK_4_LOWER('i', 'n', 'd', 'e'):  KEYWORD(5, "x",   TOKEN_KEYWORD_INDEX);
    case PACK_4_LOWER('o', 'n', 0, 0):      KEYWORD(2, "",    TOKEN_KEYWORD_ON);
    case PACK_4_LOWER('j', 'o', 'i', 'n'):  KEYWORD(4, "",    TOKEN_KEYWORD_JOIN);
    case PACK_4_LOWER('i', 'n', 'n', 'e'):  KEYWORD(5, "r",   TOKEN_KEYWORD_INNER);
    case PACK_4_LOWER('a', 's', 0, 0):      KEYWORD(2, "",    TOKEN_KEYWORD_AS);
    case PACK_4_LOWER('b', 'e', 'g', 'i'):  KEYWORD(5, "n",   TOKEN_KEYWORD_BEGIN);
    case PACK_4_LOWER('c', 'o', 'm', 'm'):  KEYWORD(6, "it",  TOKEN_KEYWORD_COMMIT);
    case PACK_4_LOWER('r', 'o', 'l', 'l'):  KEYWORD(8, "back", TOKEN_KEYWORD_ROLLBACK);
    case PACK_4_LOWER('f', 'l', 'o', 'a'):  KEYWORD(5, "t",   TOKEN_KEYWORD_FLOAT);
    case PACK_4_LOWER('d', 'a', 't', 'e'):  KEYWORD(4, "",    TOKEN_KEYWORD_DATE);
    case PACK_4_LOWER('v', 'a', 'r', 'c'):  KEYWORD(7, "har", TOKEN_KEYWORD_VARCHAR);
    case PACK_4_LOWER('p', 'r', 'i', 'm'):  KEYWORD(7, "ary", TOKEN_KEYWORD_PRIMARY);
    case PACK_4_LOWER('k', 'e', 'y', 0):    KEYWORD(3, "",    TOKEN_KEYWORD_KEY);
    case PACK_4_LOWER('u', 'n', 'i', 'q'):  KEYWORD(6, "ue",  TOKEN_KEYWORD_UNIQUE);
    case PACK_4_LOWER('d', 'e', 'f', 'a'):  KEYWORD(7, "ult", TOKEN_KEYWORD_DEFAULT);
    case PACK_4_LOWER('c', 'h', 'e', 'c'):  KEYWORD(5, "k",   TOKEN_KEYWORD_CHECK);
    case PACK_4_LOWER('a', 'l', 't', 'e'):  KEYWORD(5, "r",   TOKEN_KEYWORD_ALTER);
    case PACK_4_LOWER('a', 'd', 'd', 0):    KEYWORD(3, "",    TOKEN_KEYWORD_ADD);
    case PACK_4_LOWER('c', 'o', 'l', 'u'):  KEYWORD(6, "mn",  TOKEN_KEYWORD_COLUMN);
    case PACK_4_LOWER('r', 'e', 'n', 'a'):  KEYWORD(6, "me",  TOKEN_KEYWORD_RENAME);
    case PACK_4_LOWER('t', 'o', 0, 0):      KEYWORD(2, "",    TOKEN_KEYWORD_TO);
    case PACK_4_LOWER('l', 'e', 'f', 't'):  KEYWORD(4, "",    TOKEN_KEYWORD_LEFT);
    case PACK_4_LOWER('o', 'u', 't', 'e'):  KEYWORD(5, "r",   TOKEN_KEYWORD_OUTER);
    case PACK_4_LOWER('i', 'n', 0, 0):      KEYWORD(2, "",    TOKEN_KEYWORD_IN);
    case PACK_4_LOWER('e', 'x', 'i', 's'):  KEYWORD(6, "ts",  TOKEN_KEYWORD_EXISTS);
    case PACK_4_LOWER('d', 'e', 'l', 'e'):  KEYWORD(6, "te",  TOKEN_KEYWORD_DELETE);
    case PACK_4_LOWER('u', 'p', 'd', 'a'):  KEYWORD(6, "te",  TOKEN_KEYWORD_UPDATE);
    case PACK_4_LOWER('s', 'e', 't', 0):    KEYWORD(3, "",    TOKEN_KEYWORD_SET);
    case PACK_4_LOWER('v', 'a', 'c', 'u'):  KEYWORD(6, "um",  TOKEN_KEYWORD_VACUUM);
    case PACK_4_LOWER('o', 'r', 'd', 'e'):  KEYWORD(5, "r",   TOKEN_KEYWORD_ORDER);
    case PACK_4_LOWER('a', 's', 'c', 0):    KEYWORD(3, "",    TOKEN_KEYWORD_ASC);
    case PACK_4_LOWER('d', 'e', 's', 'c'):  KEYWORD(4, "",    TOKEN_KEYWORD_DESC);
    case PACK_4_LOWER('l', 'i', 'k', 'e'):  KEYWORD(4, "",    TOKEN_KEYWORD_LIKE);
    case PACK_4_LOWER('a', 'n', 'd', 0):    KEYWORD(3, "",    TOKEN_KEYWORD_AND);
    case PACK_4_LOWER('o', 'r', 0, 0):      KEYWORD(2, "",    TOKEN_KEYWORD_OR);
    case PACK_4_LOWER('l', 'i', 'm', 'i'):  KEYWORD(5, "t",   TOKEN_KEYWORD_LIMIT);
    case PACK_4_LOWER('d', 'i', 's', 't'):  KEYWORD(8, "inct", TOKEN_KEYWORD_DISTINCT);
    case PACK_4_LOWER('h', 'a', 'v', 'i'):  KEYWORD(6, "ng",  TOKEN_KEYWORD_HAVING);
    case PACK_4_LOWER('d', 'r', 'o', 'p'):  KEYWORD(4, "",    TOKEN_KEYWORD_DROP);
    case PACK_4_LOWER('d', 'a', 't', 'a'):  KEYWORD(8, "base", TOKEN_KEYWORD_DATABASE);
    case PACK_4_LOWER('u', 's', 'e', 0):    KEYWORD(3, "",    TOKEN_KEYWORD_USE);
    case PACK_4_LOWER('c', 'r', 'e', 'a'):  KEYWORD(6, "te",  TOKEN_KEYWORD_CREATE);
    case PACK_4_LOWER('t', 'a', 'b', 'l'):  KEYWORD(5, "e",   TOKEN_KEYWORD_TABLE);
    case PACK_4_LOWER('i', 'n', 's', 'e'):  KEYWORD(6, "rt",  TOKEN_KEYWORD_INSERT);
    case PACK_4_LOWER('i', 'n', 't', 'o'):  KEYWORD(4, "",    TOKEN_KEYWORD_INTO);
    case PACK_4_LOWER('v', 'a', 'l', 'u'):  KEYWORD(6, "es",  TOKEN_KEYWORD_VALUES);
    case PACK_4_LOWER('i', 'n', 't', 0):    KEYWORD(3, "",    TOKEN_KEYWORD_INT);
    case PACK_4_LOWER('t', 'e', 'x', 't'):  KEYWORD(4, "",    TOKEN_KEYWORD_TEXT);

    default:                            /* not a keyword prefix: one dispatch */
        return TOKEN_IDENTIFIER;
    }
}

static int pushToken(TokenList* out, TokenType type, const char* value)
{
    if (out->count == MAX_TOKENS)
        return ERROR_TOO_MANY_TOKENS;

    /* Interned rather than copied into the token: a string literal is as long
       as the line allows, and the parser outlives the buffer it was read into. */
    const char* held = internText(value, (int)strlen(value));
    if (held == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    out->tokens[out->count].type  = type;
    out->tokens[out->count].value = held;
    out->count++;
    return SUCCESS_CODE;
}

/*
 * Reads a 'quoted string', where '' is an escaped single quote.
 * Advances *index past the closing quote.
 */
static int readString(const char* sql, int* index, char* buffer)
{
    int i = *index + ONE;                       /* skip the opening quote */
    int n = ZERO;

    for (;;) {
        if (sql[i] == '\0')
            return ERROR_UNTERMINATED_STRING;

        if (sql[i] == '\'') {
            if (sql[i + ONE] != '\'')
                break;                          /* closing quote */
            i++;                                /* '' -> one literal quote */
        }

        if (n >= VALUE_LEN - ONE)               /* refuse, never shorten */
            return ERROR_TOKEN_TOO_LONG;

        buffer[n++] = sql[i];
        i++;
    }

    buffer[n] = '\0';
    *index = i + ONE;                           /* skip the closing quote */
    return SUCCESS_CODE;
}

/*
 * Reads a comparison operator: = < <= > >= <> !=
 * Two-character forms are checked first, so <= never lexes as < then =.
 */
static int readOperator(const char* sql, int* index, TokenList* out)
{
    int       i = *index;
    char      text[3] = { sql[i], sql[i + ONE], '\0' };
    TokenType type;
    int       length = TWO;

    if (strcmp(text, "<=") == ZERO)      type = TOKEN_OPERATOR_LTE;
    else if (strcmp(text, ">=") == ZERO) type = TOKEN_OPERATOR_GTE;
    else if (strcmp(text, "<>") == ZERO) type = TOKEN_OPERATOR_NE;
    else if (strcmp(text, "!=") == ZERO) type = TOKEN_OPERATOR_NE;
    else {
        length = ONE;
        text[ONE] = '\0';
        if (sql[i] == '<')      type = TOKEN_OPERATOR_LT;
        else if (sql[i] == '>') type = TOKEN_OPERATOR_GT;
        else if (sql[i] == '=') type = TOKEN_OPERATOR_EQ;
        else                    return ERROR_UNKNOWN_TOKEN;   /* a lone ! */
    }

    *index = i + length;
    return pushToken(out, type, text);
}

/*
 * Splits one statement into tokens.
 */
int tokenizeStatement(const char* sql, TokenList* out)
{
    char buffer[VALUE_LEN];
    int  i = ZERO;
    int  errorCode;

    out->count = ZERO;

    while (sql[i] != '\0') {
        unsigned char c = (unsigned char)sql[i];

        if (isspace(c)) {
            i++;
            continue;
        }

        /* -- comment: everything to end of line is not the statement's problem.
           Checked before the operator scan, which would otherwise read two
           minus signs. */
        if (c == '-' && sql[i + ONE] == '-')
            break;

        if (isalpha(c) || c == '_') {
            int n = ZERO;
            /* A dot between two identifier characters is part of the word, so
               "users.id" arrives downstream as one qualified column name and
               nothing after the lexer has to know about qualification. */
            while (isalnum((unsigned char)sql[i]) || sql[i] == '_'
                   || (sql[i] == '.' && (isalnum((unsigned char)sql[i + ONE])
                                         || sql[i + ONE] == '_'))) {
                if (n >= VALUE_LEN - ONE)
                    return ERROR_TOKEN_TOO_LONG;

                buffer[n++] = sql[i];
                i++;
            }
            buffer[n] = '\0';
            errorCode = pushToken(out, classifyWord(buffer, (size_t)n), buffer);
        }
        else if (isdigit(c)) {
            int n   = ZERO;
            int dot = ZERO;

            /* One decimal point makes it a float literal, and only one: the
               dot in "1.2.3" ends the number, and what follows is then an
               unknown token - which is what it is. */
            while (isdigit((unsigned char)sql[i])
                   || (sql[i] == '.' && !dot
                       && isdigit((unsigned char)sql[i + ONE]))) {
                if (sql[i] == '.')
                    dot = ONE;
                if (n >= VALUE_LEN - ONE)
                    return ERROR_TOKEN_TOO_LONG;

                buffer[n++] = sql[i];
                i++;
            }
            buffer[n] = '\0';
            errorCode = pushToken(out, TOKEN_NUMBER, buffer);
        }
        else if (c == '\'') {
            errorCode = readString(sql, &i, buffer);
            if (errorCode == SUCCESS_CODE)
                errorCode = pushToken(out, TOKEN_STRING, buffer);
        }
        else if (c == '<' || c == '>' || c == '=' || c == '!') {
            errorCode = readOperator(sql, &i, out);
        }
        else {
            TokenType type;
            switch (c) {
            case '*': type = TOKEN_STAR;      break;
            case ',': type = TOKEN_COMMA;     break;
            case '(': type = TOKEN_LPAREN;    break;
            case ')': type = TOKEN_RPAREN;    break;
            case ';': type = TOKEN_SEMICOLON; break;
            case '-': type = TOKEN_MINUS;     break;
            case '+': type = TOKEN_PLUS;      break;
            case '/': type = TOKEN_SLASH;     break;
            case '%': type = TOKEN_PERCENT;   break;
            default:  return ERROR_UNKNOWN_TOKEN;
            }
            buffer[ZERO] = (char)c;
            buffer[ONE]  = '\0';
            i++;
            errorCode = pushToken(out, type, buffer);
        }

        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    return SUCCESS_CODE;
}
