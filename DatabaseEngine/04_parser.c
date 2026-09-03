#include "sql_common.h"
#include <errno.h>
#include <limits.h>

/*
 * Type of token i, or TOKEN_UNKNOWN past the end. The lexer rejects a real
 * TOKEN_UNKNOWN before parsing, so it is safe to use as the end sentinel.
 */
static TokenType typeAt(const TokenList* tokens, int index)
{
    return index < tokens->count ? tokens->tokens[index].type : TOKEN_UNKNOWN;
}

static int parseSelect(const TokenList* tokens, SelectStatement* out);

/*
 * Subqueries.
 *
 * A SelectStatement is about 18 KB, so a statement cannot hold the ones it
 * mentions - the type would be recursive and the statement enormous. They live
 * here instead and a Predicate keeps an index, which also means a Condition is
 * still a flat thing that copies by value.
 *
 * The pool is emptied at the start of every statement, so an index is valid
 * for exactly as long as the statement that parsed it - the same lifetime the
 * statement arena gives to text.
 */
static THREAD_LOCAL SelectStatement subqueryPool[MAX_SUBQUERIES];
static THREAD_LOCAL int             subqueryScalar[MAX_SUBQUERIES];
static THREAD_LOCAL int             subqueryCount;

void resetSubqueries(void)
{
    subqueryCount = ZERO;
}

int subqueryTotal(void)
{
    return subqueryCount;
}

/* Set where the parser knows it: on the right of a comparison operator, one
   value is wanted, and more than one row back is an error rather than a set. */
int subqueryIsScalar(int index)
{
    return index >= ZERO && index < subqueryCount ? subqueryScalar[index] : ZERO;
}

SelectStatement* subqueryAt(int index)
{
    return index >= ZERO && index < subqueryCount ? &subqueryPool[index] : NULL;
}

/*
 * ( select ... ) in a position where a value or a set is expected.
 */
static int parseSubquery(const TokenList* tokens, int* index, int* out)
{
    int i = *index;

    if (typeAt(tokens, i) != TOKEN_LPAREN
        || typeAt(tokens, i + ONE) != TOKEN_KEYWORD_SELECT)
        return ERROR_SYNTAX_EXPECTED_PARENTHESES;

    if (subqueryCount == MAX_SUBQUERIES)
        return ERROR_SYNTAX_TOO_MANY_SUBQUERIES;

    i++;                                        /* past the '(' */

    /* parseSelect wants a statement of its own, and reads to the end of the
       token list - so the inner SELECT is lifted into a list of its own,
       stopping at the parenthesis that closes it.
       A local rather than a static: this recurses for a nested subquery, and a
       shared one would be overwritten by the inner call while the outer
       parseSelect was still reading it. At 1.6 KB and a depth bounded by
       MAX_SUBQUERIES, the stack is the right place for it. */
    TokenList inner;
    int       depth = ONE;
    int       at    = i;

    inner.count = ZERO;

    while (at < tokens->count) {
        TokenType type = tokens->tokens[at].type;

        if (type == TOKEN_LPAREN)
            depth++;
        else if (type == TOKEN_RPAREN && --depth == ZERO)
            break;

        if (inner.count == MAX_TOKENS)
            return ERROR_TOO_MANY_TOKENS;

        inner.tokens[inner.count++] = tokens->tokens[at++];
    }

    if (depth != ZERO)
        return ERROR_SYNTAX_EXPECTED_PARENTHESES;

    int slot = subqueryCount++;

    subqueryScalar[slot] = ZERO;

    int errorCode = parseSelect(&inner, &subqueryPool[slot]);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    *out   = slot;
    *index = at + ONE;                          /* past the ')' */
    return SUCCESS_CODE;
}

/*
 * Accepts an optional trailing semicolon, then requires the end of input.
 */
static int endOfStatement(const TokenList* tokens, int index)
{
    if (typeAt(tokens, index) == TOKEN_SEMICOLON)
        index++;

    return index == tokens->count ? SUCCESS_CODE : ERROR_SYNTAX_TRAILING_TOKENS;
}

/*
 * Reads one literal: an optionally negated number, a quoted string, or NULL.
 * Shared by INSERT ... VALUES and by the WHERE clause.
 */
static int parseValue(const TokenList* tokens, int* index, Value* out)
{
    int i        = *index;
    int negative = ZERO;

    /* Cleared whole rather than field by field: a number sets the type and the
       number and nothing else, and a Value carrying a stale text pointer is a
       trap for anything that later reads it by type - as writing a DEFAULT
       into the catalog does. */
    *out = (Value){ ZERO };

    if (typeAt(tokens, i) == TOKEN_MINUS) {
        negative = ONE;
        i++;
    }

    if (typeAt(tokens, i) == TOKEN_NUMBER) {
        const char* digits = tokens->tokens[i].value;

        /* The lexer only puts a dot in a number when it was written as one, so
           the dot is the whole decision: 3 is an int and 3.0 is a float. */
        if (strchr(digits, '.') != NULL) {
            errno = ZERO;

            double number = strtod(digits, NULL);
            if (errno == ERANGE)
                return ERROR_VALUE_OUT_OF_RANGE;

            setFloat(out, negative ? -number : number);
        }
        else {
            errno = ZERO;
            long long n = strtoll(digits, NULL, 10);
            if (negative)
                n = -n;

            /* long is 32-bit on Windows, so the range check must use long long */
            if (errno == ERANGE || n < INT_MIN || n > INT_MAX)
                return ERROR_VALUE_OUT_OF_RANGE;

            out->type     = TYPE_INT;
            out->intValue = (int)n;
        }
    }
    else if (!negative && typeAt(tokens, i) == TOKEN_STRING) {
        /* the token is already interned, so the value can share it */
        out->type       = TYPE_TEXT;
        out->isNull     = ZERO;
        out->text       = tokens->tokens[i].value;
        out->textLength = (int)strlen(tokens->tokens[i].value);
    }
    else if (!negative && typeAt(tokens, i) == TOKEN_KEYWORD_NULL) {
        /* the column being written decides the type; NULL carries none */
        setNull(out, TYPE_INT);
    }
    else {
        return ERROR_SYNTAX_EXPECTED_VALUE;
    }

    *index = i + ONE;
    return SUCCESS_CODE;
}

/*
 * Allocates a node in the condition pool.
 */
static int newCondition(Condition* out, ConditionKind kind, int* node)
{
    if (out->count == MAX_CONDITION_NODES)
        return ERROR_SYNTAX_CONDITION_TOO_COMPLEX;

    *node = out->count++;
    out->nodes[*node].kind  = kind;
    out->nodes[*node].left  = -1;
    out->nodes[*node].right = -1;
    return SUCCESS_CODE;
}

/*
 * Expressions: the arithmetic a query is allowed to do on its way to a value.
 *
 *   expression := term (('+' | '-') term)*
 *   term       := factor (('*' | '/' | '%') factor)*
 *   factor     := '-' factor | '(' expression ')' | <literal> | <column>
 *
 * Two levels, so that multiplication binds tighter than addition, and factor
 * recurses for parentheses and for a leading minus. Nothing here knows what a
 * column is worth - the semantic stage binds them and the executor reads them.
 */
static int parseExpression(const TokenList* tokens, int* index,
                           ExprPool* pool, int* node);

static int parseFactor(const TokenList* tokens, int* index,
                       ExprPool* pool, int* node)
{
    int i = *index;

    if (typeAt(tokens, i) == TOKEN_MINUS) {
        int inner;

        i++;                            /* past the minus, or this reads it again */

        int errorCode = parseFactor(tokens, &i, pool, &inner);

        if (errorCode != SUCCESS_CODE)
            return errorCode;

        errorCode = exprNew(pool, EXPR_NEGATE, node);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        pool->nodes[*node].left = inner;
        *index = i;
        return SUCCESS_CODE;
    }

    if (typeAt(tokens, i) == TOKEN_LPAREN) {
        i++;

        int errorCode = parseExpression(tokens, &i, pool, node);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        if (typeAt(tokens, i++) != TOKEN_RPAREN)
            return ERROR_SYNTAX_EXPECTED_PARENTHESES;

        *index = i;
        return SUCCESS_CODE;
    }

    /* A bare name is a column. An aggregate is not parsed here: it wraps an
       expression rather than sitting inside one, and only a select item or a
       HAVING term may write one. */
    if (typeAt(tokens, i) == TOKEN_IDENTIFIER) {
        int errorCode = exprNew(pool, EXPR_COLUMN, node);

        if (errorCode != SUCCESS_CODE)
            return errorCode;

        snprintf(pool->nodes[*node].column, NAME_LEN, "%s",
                 tokens->tokens[i].value);
        *index = i + ONE;
        return SUCCESS_CODE;
    }

    switch (typeAt(tokens, i)) {
    case TOKEN_NUMBER:
    case TOKEN_STRING:
    case TOKEN_KEYWORD_NULL:
        break;

    default:
        /* Not a name and not a literal, so the thing that is missing is a
           column - which is what the reader needs to be told. */
        return ERROR_SYNTAX_EXPECTED_COLUMN;
    }

    int errorCode = exprNew(pool, EXPR_LITERAL, node);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    return parseValue(tokens, index, &pool->nodes[*node].literal);
}

static int parseTerm(const TokenList* tokens, int* index,
                     ExprPool* pool, int* node)
{
    int errorCode = parseFactor(tokens, index, pool, node);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    for (;;) {
        ArithOp op;

        switch (typeAt(tokens, *index)) {
        case TOKEN_STAR:    op = ARITH_MUL; break;
        case TOKEN_SLASH:   op = ARITH_DIV; break;
        case TOKEN_PERCENT: op = ARITH_MOD; break;
        default:            return SUCCESS_CODE;
        }

        int i = *index + ONE;
        int right;

        errorCode = parseFactor(tokens, &i, pool, &right);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        int parent;

        errorCode = exprNew(pool, EXPR_BINARY, &parent);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        pool->nodes[parent].op    = op;
        pool->nodes[parent].left  = *node;
        pool->nodes[parent].right = right;
        *node  = parent;
        *index = i;
    }
}

static int parseExpression(const TokenList* tokens, int* index,
                           ExprPool* pool, int* node)
{
    int errorCode = parseTerm(tokens, index, pool, node);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    for (;;) {
        ArithOp op;

        switch (typeAt(tokens, *index)) {
        case TOKEN_PLUS:  op = ARITH_ADD; break;
        case TOKEN_MINUS: op = ARITH_SUB; break;
        default:          return SUCCESS_CODE;
        }

        int i = *index + ONE;
        int right;

        errorCode = parseTerm(tokens, &i, pool, &right);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        int parent;

        errorCode = exprNew(pool, EXPR_BINARY, &parent);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        pool->nodes[parent].op    = op;
        pool->nodes[parent].left  = *node;
        pool->nodes[parent].right = right;
        *node  = parent;
        *index = i;
    }
}

/* A literal on its own, as its own expression node. */
static int literalExpr(ExprPool* pool, const Value* value, int* node)
{
    int errorCode = exprNew(pool, EXPR_LITERAL, node);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    pool->nodes[*node].literal = *value;
    return SUCCESS_CODE;
}

/* A column named directly, for a HAVING term that matches an output header. */
static int columnExpr(ExprPool* pool, const char* name, int* node)
{
    int errorCode = exprNew(pool, EXPR_COLUMN, node);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    snprintf(pool->nodes[*node].column, NAME_LEN, "%s", name);
    return SUCCESS_CODE;
}

static int parseSelectItem(const TokenList* tokens, int* index,
                           ExprPool* pool, SelectItem* out);

static int parseOrExpression(const TokenList* tokens, int* index,
                             Condition* out, int* node, int allowAggregate);

/*
 * <column> <op> <literal> | <column> IS [NOT] NULL | <column> [NOT] LIKE <text>
 */
static int parseComparison(const TokenList* tokens, int* index,
                           Condition* out, int* node, int allowAggregate)
{
    int i    = *index;
    int left = -1;
    int errorCode;

    if (allowAggregate) {
        /* HAVING names result columns rather than table columns, so the left
           side is a select item and the label it produces is what gets matched
           against the output headers. The item's own expression is built and
           left behind; only its name is wanted here. */
        SelectItem item;

        /* Parsed into a pool of its own and thrown away: only the label
           survives, and the condition's pool stays free of operands nothing
           will ever evaluate. Static because it is large and this is never
           re-entered - parseSelectItem cannot reach another comparison. */
        /* Per thread: parsing deliberately runs before the engine lock is
           taken, so two connections are in here at once. */
        static THREAD_LOCAL ExprPool discard;

        exprInit(&discard);

        errorCode = parseSelectItem(tokens, &i, &discard, &item);
        if (errorCode == SUCCESS_CODE)
            errorCode = columnExpr(&out->exprs, item.label, &left);
    }
    else {
        errorCode = parseExpression(tokens, &i, &out->exprs, &left);
    }

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    errorCode = newCondition(out, COND_COMPARE, node);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    Predicate* compare = &out->nodes[*node].compare;

    compare->left     = left;
    compare->right    = -1;                 /* IS NULL never fills it in */
    compare->subquery = -1;                 /* set only by IN and EXISTS */

    /* [NOT] IN: either a subquery, or a list of values.
     *
     * The list form is rewritten into "x = a OR x = b OR ...", which is not a
     * shortcut but the definition - including what it does with NULL, since an
     * OR of unknowns is unknown exactly as IN against a NULL is. The subquery
     * form cannot be rewritten that way because its values are not known until
     * it runs, so that one gets an operator of its own.
     */
    if (typeAt(tokens, i) == TOKEN_KEYWORD_IN
        || (typeAt(tokens, i) == TOKEN_KEYWORD_NOT
            && typeAt(tokens, i + ONE) == TOKEN_KEYWORD_IN)) {

        int negated = typeAt(tokens, i) == TOKEN_KEYWORD_NOT;

        i += negated ? TWO : ONE;

        if (typeAt(tokens, i + ONE) == TOKEN_KEYWORD_SELECT) {
            compare->op = OP_IN;

            errorCode = parseSubquery(tokens, &i, &compare->subquery);
            if (errorCode != SUCCESS_CODE)
                return errorCode;
        }
        else {
            if (typeAt(tokens, i++) != TOKEN_LPAREN)
                return ERROR_SYNTAX_EXPECTED_PARENTHESES;

            /* The node already allocated becomes the first equality; each
               further value adds another and an OR above it. */
            int spine = *node;

            for (;;) {
                Predicate* term = &out->nodes[spine].compare;

                if (spine != *node) {
                    term->left     = left;
                    term->subquery = -1;
                }

                term->op = OP_EQ;

                errorCode = parseExpression(tokens, &i, &out->exprs, &term->right);
                if (errorCode != SUCCESS_CODE)
                    return errorCode;

                if (typeAt(tokens, i) != TOKEN_COMMA)
                    break;
                i++;

                int next;
                errorCode = newCondition(out, COND_COMPARE, &next);
                if (errorCode != SUCCESS_CODE)
                    return errorCode;

                int either;
                errorCode = newCondition(out, COND_OR, &either);
                if (errorCode != SUCCESS_CODE)
                    return errorCode;

                out->nodes[either].left  = *node;
                out->nodes[either].right = next;
                *node = either;
                spine = next;
            }

            if (typeAt(tokens, i++) != TOKEN_RPAREN)
                return ERROR_SYNTAX_EXPECTED_PARENTHESES;
        }

        if (negated) {
            int inverted;

            errorCode = newCondition(out, COND_NOT, &inverted);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            out->nodes[inverted].left = *node;
            *node = inverted;
        }

        *index = i;
        return SUCCESS_CODE;
    }

    if (typeAt(tokens, i) == TOKEN_KEYWORD_IS) {
        i++;
        compare->op = OP_IS_NULL;

        if (typeAt(tokens, i) == TOKEN_KEYWORD_NOT) {
            compare->op = OP_IS_NOT_NULL;
            i++;
        }

        if (typeAt(tokens, i++) != TOKEN_KEYWORD_NULL)
            return ERROR_SYNTAX_EXPECTED_NULL;

        *index = i;
        return SUCCESS_CODE;
    }

    if (typeAt(tokens, i) == TOKEN_KEYWORD_LIKE
        || (typeAt(tokens, i) == TOKEN_KEYWORD_NOT
            && typeAt(tokens, i + ONE) == TOKEN_KEYWORD_LIKE)) {

        compare->op = OP_LIKE;
        if (typeAt(tokens, i) == TOKEN_KEYWORD_NOT) {
            compare->op = OP_NOT_LIKE;
            i++;
        }
        i++;

        /* A pattern is a constant, not an expression: it is read once and
           walked character by character, and there is nothing to compute. */
        Value pattern;

        errorCode = parseValue(tokens, &i, &pattern);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        if (pattern.isNull || pattern.type != TYPE_TEXT)
            return ERROR_SYNTAX_EXPECTED_VALUE;

        errorCode = literalExpr(&out->exprs, &pattern, &compare->right);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        *index = i;
        return SUCCESS_CODE;
    }

    switch (typeAt(tokens, i)) {
    case TOKEN_OPERATOR_EQ:  compare->op = OP_EQ;  break;
    case TOKEN_OPERATOR_NE:  compare->op = OP_NE;  break;
    case TOKEN_OPERATOR_LT:  compare->op = OP_LT;  break;
    case TOKEN_OPERATOR_LTE: compare->op = OP_LTE; break;
    case TOKEN_OPERATOR_GT:  compare->op = OP_GT;  break;
    case TOKEN_OPERATOR_GTE: compare->op = OP_GTE; break;
    default: return ERROR_SYNTAX_EXPECTED_OPERATOR;
    }
    i++;

    /* "x = (select ...)" compares against whatever the subquery produced, so
       the right side is a subquery rather than an expression. It has to return
       one row, which is not knowable here - the executor says so. */
    if (typeAt(tokens, i) == TOKEN_LPAREN
        && typeAt(tokens, i + ONE) == TOKEN_KEYWORD_SELECT) {

        errorCode = parseSubquery(tokens, &i, &compare->subquery);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        subqueryScalar[compare->subquery] = ONE;

        *index = i;
        return SUCCESS_CODE;
    }

    /* Whatever is on the right is an expression too, so a literal, another
       column and "b + 1" are the same case. */
    errorCode = parseExpression(tokens, &i, &out->exprs, &compare->right);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /* x = NULL is never true in SQL; require IS NULL so it is not a silent bug */
    Value* literal = exprLiteral(&out->exprs, compare->right);

    if (literal != NULL && literal->isNull)
        return ERROR_SYNTAX_EXPECTED_NULL;

    *index = i;
    return SUCCESS_CODE;
}

/*
 * ( <condition> ) | <comparison>
 */
static int parsePrimary(const TokenList* tokens, int* index,
                        Condition* out, int* node, int allowAggregate)
{
    /* EXISTS is a condition on its own rather than a comparison, so it is
       recognised here instead of in parseComparison: there is no left side. */
    if (typeAt(tokens, *index) == TOKEN_KEYWORD_EXISTS) {
        int i = *index + ONE;

        int errorCode = newCondition(out, COND_COMPARE, node);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        Predicate* compare = &out->nodes[*node].compare;

        compare->left  = -1;
        compare->right = -1;
        compare->op    = OP_EXISTS;

        errorCode = parseSubquery(tokens, &i, &compare->subquery);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        *index = i;
        return SUCCESS_CODE;
    }

    if (typeAt(tokens, *index) == TOKEN_LPAREN) {
        int i = *index + ONE;

        int errorCode = parseOrExpression(tokens, &i, out, node, allowAggregate);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        if (typeAt(tokens, i++) != TOKEN_RPAREN)
            return ERROR_SYNTAX_EXPECTED_PARENTHESES;

        *index = i;
        return SUCCESS_CODE;
    }

    return parseComparison(tokens, index, out, node, allowAggregate);
}

/*
 * NOT binds tighter than AND. A NOT here is always logical: the NOT in
 * "x NOT LIKE" and "IS NOT NULL" is consumed inside parseComparison.
 */
static int parseNotExpression(const TokenList* tokens, int* index,
                              Condition* out, int* node, int allowAggregate)
{
    if (typeAt(tokens, *index) != TOKEN_KEYWORD_NOT)
        return parsePrimary(tokens, index, out, node, allowAggregate);

    int i = *index + ONE;
    int child;

    int errorCode = parseNotExpression(tokens, &i, out, &child, allowAggregate);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    errorCode = newCondition(out, COND_NOT, node);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    out->nodes[*node].left = child;
    *index = i;
    return SUCCESS_CODE;
}

static int parseAndExpression(const TokenList* tokens, int* index,
                              Condition* out, int* node, int allowAggregate)
{
    int errorCode = parseNotExpression(tokens, index, out, node, allowAggregate);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    while (typeAt(tokens, *index) == TOKEN_KEYWORD_AND) {
        int i = *index + ONE;
        int right;

        errorCode = parseNotExpression(tokens, &i, out, &right, allowAggregate);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        int parent;
        errorCode = newCondition(out, COND_AND, &parent);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        out->nodes[parent].left  = *node;
        out->nodes[parent].right = right;
        *node  = parent;
        *index = i;
    }

    return SUCCESS_CODE;
}

/*
 * OR binds loosest, so it sits at the top of the tree.
 */
static int parseOrExpression(const TokenList* tokens, int* index,
                             Condition* out, int* node, int allowAggregate)
{
    int errorCode = parseAndExpression(tokens, index, out, node, allowAggregate);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    while (typeAt(tokens, *index) == TOKEN_KEYWORD_OR) {
        int i = *index + ONE;
        int right;

        errorCode = parseAndExpression(tokens, &i, out, &right, allowAggregate);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        int parent;
        errorCode = newCondition(out, COND_OR, &parent);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        out->nodes[parent].left  = *node;
        out->nodes[parent].right = right;
        *node  = parent;
        *index = i;
    }

    return SUCCESS_CODE;
}

/*
 * Parses the whole WHERE clause, keeping the same call shape the statement
 * parsers already use.
 */
static int parseHaving(const TokenList* tokens, int* index, Condition* out)
{
    int i = *index + ONE;                               /* skip HAVING */

    out->count   = ZERO;
    out->present = ZERO;
    exprInit(&out->exprs);

    int root;
    int errorCode = parseOrExpression(tokens, &i, out, &root, ONE);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    out->root    = root;
    out->present = ONE;
    *index = i;
    return SUCCESS_CODE;
}

/*
 * Parses a condition into a pool that may already hold nodes, ANDing it onto
 * the root built so far. JOIN ... ON and WHERE both come through here, so
 * "a join b on a.id = b.uid where total > 100" ends up as one tree over one
 * pool - which is what lets the executor treat ON as ordinary filtering.
 */
static int addCondition(const TokenList* tokens, int* index,
                        Condition* out, int* root)
{
    int node;
    int errorCode = parseOrExpression(tokens, index, out, &node, ZERO);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    if (*root < ZERO) {
        *root = node;
        return SUCCESS_CODE;
    }

    int combined;
    errorCode = newCondition(out, COND_AND, &combined);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    out->nodes[combined].left  = *root;
    out->nodes[combined].right = node;
    *root = combined;
    return SUCCESS_CODE;
}

static int parseWhere(const TokenList* tokens, int* index, Condition* out)
{
    int i = *index + ONE;                               /* skip WHERE */

    out->count   = ZERO;
    out->present = ZERO;
    exprInit(&out->exprs);

    int root;
    int errorCode = parseOrExpression(tokens, &i, out, &root, ZERO);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    out->root    = root;
    out->present = ONE;
    *index = i;
    return SUCCESS_CODE;
}

/*
 * int | text | float | date | varchar ( <n> )
 *
 * varchar is text with a ceiling rather than a type of its own, so everything
 * below the catalog goes on seeing two string cases instead of three.
 */
static int parseColumnType(const TokenList* tokens, int* index, Column* out)
{
    int i = *index;

    out->size = ZERO;

    switch (typeAt(tokens, i)) {
    case TOKEN_KEYWORD_INT:   out->type = TYPE_INT;   break;
    case TOKEN_KEYWORD_TEXT:  out->type = TYPE_TEXT;  break;
    case TOKEN_KEYWORD_FLOAT: out->type = TYPE_FLOAT; break;
    case TOKEN_KEYWORD_DATE:  out->type = TYPE_DATE;  break;

    case TOKEN_KEYWORD_VARCHAR: {
        out->type = TYPE_TEXT;
        i++;

        if (typeAt(tokens, i++) != TOKEN_LPAREN)
            return ERROR_SYNTAX_EXPECTED_PARENTHESES;

        if (typeAt(tokens, i) != TOKEN_NUMBER)
            return ERROR_SYNTAX_EXPECTED_SIZE;

        long long size = strtoll(tokens->tokens[i++].value, NULL, 10);
        if (size <= ZERO || size > VALUE_LEN)
            return ERROR_SYNTAX_EXPECTED_SIZE;

        out->size = (int)size;

        if (typeAt(tokens, i++) != TOKEN_RPAREN)
            return ERROR_SYNTAX_EXPECTED_PARENTHESES;

        *index = i;
        return SUCCESS_CODE;
    }

    default:
        return ERROR_SYNTAX_EXPECTED_TYPE;
    }

    *index = i + ONE;
    return SUCCESS_CODE;
}

/*
 * Whatever follows a column's type: PRIMARY KEY, UNIQUE, NOT NULL,
 * DEFAULT <literal>, CHECK ( <condition> ) - in any order and any number.
 *
 * A CHECK does not belong to the column it was written on. It is ANDed into
 * the table's one condition tree exactly as a table-level CHECK is, because
 * "age > 0" written beside a column and written after the last one mean the
 * same thing, and there is no reason to hold them two ways.
 */
static int parseColumnConstraints(const TokenList* tokens, int* index,
                                  Column* out, Condition* check, int* checkRoot)
{
    for (;;) {
        int i = *index;

        switch (typeAt(tokens, i)) {
        case TOKEN_KEYWORD_PRIMARY:
            if (typeAt(tokens, i + ONE) != TOKEN_KEYWORD_KEY)
                return ERROR_SYNTAX_EXPECTED_KEY;

            /* a primary key is unique and present; the flag records only
               which of the three the statement actually said */
            out->flags |= COL_PRIMARY | COL_UNIQUE | COL_NOT_NULL;
            *index = i + TWO;
            break;

        case TOKEN_KEYWORD_UNIQUE:
            out->flags |= COL_UNIQUE;
            *index = i + ONE;
            break;

        case TOKEN_KEYWORD_NOT:
            if (typeAt(tokens, i + ONE) != TOKEN_KEYWORD_NULL)
                return ERROR_SYNTAX_EXPECTED_NULL;

            out->flags |= COL_NOT_NULL;
            *index = i + TWO;
            break;

        case TOKEN_KEYWORD_DEFAULT: {
            i++;

            int errorCode = parseValue(tokens, &i, &out->defaultValue);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            out->hasDefault = ONE;
            *index = i;
            break;
        }

        case TOKEN_KEYWORD_CHECK: {
            i++;

            if (typeAt(tokens, i++) != TOKEN_LPAREN)
                return ERROR_SYNTAX_EXPECTED_PARENTHESES;

            int errorCode = addCondition(tokens, &i, check, checkRoot);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            if (typeAt(tokens, i++) != TOKEN_RPAREN)
                return ERROR_SYNTAX_EXPECTED_PARENTHESES;

            *index = i;
            break;
        }

        default:
            return SUCCESS_CODE;                /* not a constraint: done */
        }
    }
}

/*
 * create table <name> ( <column> <type> [<constraint>]* [, ...] [, check (...)] )
 */
static int parseCreateTable(const TokenList* tokens, CreateStatement* out)
{
    int index = ONE;                                    /* CREATE is token 0 */

    if (typeAt(tokens, index++) != TOKEN_KEYWORD_TABLE)
        return ERROR_SYNTAX_INVALID_STATEMENT;

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(out->table, NAME_LEN, "%s", tokens->tokens[index++].value);

    if (typeAt(tokens, index++) != TOKEN_LPAREN)
        return ERROR_SYNTAX_EXPECTED_PARENTHESES;

    out->ncols         = ZERO;
    out->check.count   = ZERO;
    out->check.present = ZERO;
    exprInit(&out->check.exprs);

    int checkRoot = -1;

    for (;;) {
        /* An entry opening with CHECK constrains the table rather than a
           column, and lands in the same tree either way. */
        if (typeAt(tokens, index) == TOKEN_KEYWORD_CHECK) {
            Column ignored = { { 0 }, TYPE_INT, ZERO, ZERO, ZERO, { 0 } };

            int errorCode = parseColumnConstraints(tokens, &index, &ignored,
                                                   &out->check, &checkRoot);
            if (errorCode != SUCCESS_CODE)
                return errorCode;
        }
        else {
            if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
                return ERROR_SYNTAX_EXPECTED_COLUMN;
            if (out->ncols == MAX_COLS)
                return ERROR_SYNTAX_TOO_MANY_COLUMNS;

            Column* column = &out->cols[out->ncols];

            *column = (Column){ { 0 }, TYPE_INT, ZERO, ZERO, ZERO, { 0 } };
            snprintf(column->name, NAME_LEN, "%s", tokens->tokens[index++].value);

            int errorCode = parseColumnType(tokens, &index, column);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            errorCode = parseColumnConstraints(tokens, &index, column,
                                               &out->check, &checkRoot);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            out->ncols++;
        }

        if (typeAt(tokens, index) != TOKEN_COMMA)
            break;
        index++;
    }

    if (out->ncols == ZERO)
        return ERROR_SYNTAX_EXPECTED_COLUMN;

    if (typeAt(tokens, index++) != TOKEN_RPAREN)
        return ERROR_SYNTAX_EXPECTED_PARENTHESES;

    if (checkRoot >= ZERO) {
        out->check.root    = checkRoot;
        out->check.present = ONE;
    }

    return endOfStatement(tokens, index);
}

static int aggregateFromName(const char* name, AggregateType* out)
{
    if (_stricmp(name, "count") == ZERO) { *out = AGG_COUNT; return ONE; }
    if (_stricmp(name, "sum")   == ZERO) { *out = AGG_SUM;   return ONE; }
    if (_stricmp(name, "avg")   == ZERO) { *out = AGG_AVG;   return ONE; }
    if (_stricmp(name, "min")   == ZERO) { *out = AGG_MIN;   return ONE; }
    if (_stricmp(name, "max")   == ZERO) { *out = AGG_MAX;   return ONE; }
    return ZERO;
}

/*
 * One select-list entry: a plain column, or count(*) / count(c) / sum(c) / min(c) / max(c).
 * An identifier followed by '(' is a function call - the same lookahead
 * parseStatement uses in the compiler to tell a call from an assignment.
 */
static int parseSelectItem(const TokenList* tokens, int* index,
                           ExprPool* pool, SelectItem* out)
{
    int i = *index;

    out->aliased   = ZERO;
    out->star      = ZERO;
    out->expr      = -1;
    out->aggregate = AGG_NONE;

    /* An identifier followed by '(' is a function call - the same lookahead the
       compiler used to tell a call from an assignment. */
    if (typeAt(tokens, i) == TOKEN_IDENTIFIER
        && typeAt(tokens, i + ONE) == TOKEN_LPAREN) {

        char name[NAME_LEN];

        snprintf(name, NAME_LEN, "%s", tokens->tokens[i].value);

        if (!aggregateFromName(name, &out->aggregate))
            return ERROR_SYNTAX_UNKNOWN_FUNCTION;

        i += TWO;                                       /* skip name and '(' */

        if (typeAt(tokens, i) == TOKEN_STAR) {
            if (out->aggregate != AGG_COUNT)
                return ERROR_SYNTAX_EXPECTED_COLUMN;    /* only count(*) exists */

            out->star = ONE;
            /* Widths, not just a buffer size: an aggregate name plus "(*)"
               can outrun the label, and saying where it is cut keeps the
               truncation deliberate rather than something the compiler has to
               warn about on every build. */
            snprintf(out->label, NAME_LEN, "%.*s(*)", NAME_LEN - 4, name);
            i++;
        }
        else {
            /* An aggregate takes an expression, so sum(price * qty) needs no
               column of its own to exist first. */
            int errorCode = parseExpression(tokens, &i, pool, &out->expr);

            if (errorCode != SUCCESS_CODE)
                return errorCode;

            char inner[NAME_LEN];

            exprLabel(pool, out->expr, inner, sizeof inner);
            snprintf(out->label, NAME_LEN, "%.*s(%.*s)",
                     NAME_LEN / TWO - TWO, name, NAME_LEN / TWO - TWO, inner);
        }

        if (typeAt(tokens, i++) != TOKEN_RPAREN)
            return ERROR_SYNTAX_EXPECTED_PARENTHESES;

        *index = i;
        return SUCCESS_CODE;
    }

    int errorCode = parseExpression(tokens, &i, pool, &out->expr);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /* Unaliased, an item is named after what it says: a column keeps its own
       name and "a*2" is called a*2, which is also what ORDER BY will look for. */
    exprLabel(pool, out->expr, out->label, NAME_LEN);

    *index = i;
    return SUCCESS_CODE;
}

/*
 * select <item> [, <item>]* from <table> [where ...] [group by <col> [, <col>]*]
 * select * from <table> [where ...]
 */
static int parseSelect(const TokenList* tokens, SelectStatement* out)
{
    int index = ONE;                                    /* SELECT is token 0 */

    out->nitems         = ZERO;
    out->selectAll      = ZERO;
    out->distinct       = ZERO;
    exprInit(&out->exprs);
    exprInit(&out->where.exprs);
    exprInit(&out->having.exprs);
    out->where.count    = ZERO;
    out->where.present  = ZERO;
    out->having.present = ZERO;
    out->ngroup         = ZERO;
    out->norder         = ZERO;
    out->limit          = -1;

    if (typeAt(tokens, index) == TOKEN_KEYWORD_DISTINCT) {
        out->distinct = ONE;
        index++;
    }

    if (typeAt(tokens, index) == TOKEN_STAR) {
        out->selectAll = ONE;
        index++;
    }
    else {
        for (;;) {
            if (out->nitems == MAX_COLS)
                return ERROR_SYNTAX_TOO_MANY_COLUMNS;

            SelectItem* item = &out->items[out->nitems];

            int errorCode = parseSelectItem(tokens, &index, &out->exprs, item);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            /* [AS] <alias> renames the output column. Done here rather than in
               parseSelectItem because HAVING parses items too, and there a
               trailing identifier would be someone else's token. */
            if (typeAt(tokens, index) == TOKEN_KEYWORD_AS)
                index++;

            item->aliased = typeAt(tokens, index) == TOKEN_IDENTIFIER;
            if (item->aliased)
                snprintf(item->label, NAME_LEN, "%s", tokens->tokens[index++].value);

            out->nitems++;

            if (typeAt(tokens, index) != TOKEN_COMMA)
                break;
            index++;
        }
    }

    if (typeAt(tokens, index++) != TOKEN_KEYWORD_FROM)
        return ERROR_SYNTAX_EXPECTED_FROM;

    /* FROM a, b and a JOIN b ON ... are the same thing: a list of tables plus a
       condition. A single table is the ordinary case and stays on the
       single-table path; the list is what the executor cross-products. */
    out->ntables = ZERO;

    int whereRoot = -1;
    int expectOn  = ZERO;
    int expectOuter = ZERO;             /* the join we are about to read is LEFT */

    for (int t = ZERO; t < MAX_JOIN_TABLES; t++) {
        out->outer[t]  = ZERO;
        out->onRoot[t] = -ONE;
    }

    for (;;) {
        if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
            return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
        if (out->ntables == MAX_JOIN_TABLES)
            return ERROR_SYNTAX_TOO_MANY_TABLES;

        int slot = out->ntables++;

        snprintf(out->tables[slot], NAME_LEN, "%s", tokens->tokens[index++].value);

        /* [AS] <alias>. Keywords are their own token types, so an identifier
           here can only be an alias - nothing else may follow a table name. */
        if (typeAt(tokens, index) == TOKEN_KEYWORD_AS)
            index++;

        if (typeAt(tokens, index) == TOKEN_IDENTIFIER)
            snprintf(out->aliases[slot], NAME_LEN, "%s", tokens->tokens[index++].value);
        else
            snprintf(out->aliases[slot], NAME_LEN, "%s", out->tables[slot]);

        if (expectOn) {
            if (typeAt(tokens, index++) != TOKEN_KEYWORD_ON)
                return ERROR_SYNTAX_EXPECTED_ON;

            /* An outer join's ON is built as its own tree in the same pool and
               kept out of the WHERE. An inner join's is ANDed in, which is
               what makes ON and WHERE interchangeable for one. */
            int  standalone = -ONE;
            int* root       = expectOuter ? &standalone : &whereRoot;

            int errorCode = addCondition(tokens, &index, &out->where, root);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            if (expectOuter) {
                out->outer[slot]  = ONE;
                out->onRoot[slot] = standalone;
            }

            expectOn    = ZERO;
            expectOuter = ZERO;
        }

        if (typeAt(tokens, index) == TOKEN_COMMA) {
            index++;
            continue;
        }

        /* [INNER] JOIN, or LEFT [OUTER] JOIN. The optional word in each is
           noise; only LEFT changes what the join means. */
        int skip = ZERO;
        int left = ZERO;

        if (typeAt(tokens, index) == TOKEN_KEYWORD_INNER) {
            skip = ONE;
        }
        else if (typeAt(tokens, index) == TOKEN_KEYWORD_LEFT) {
            left = ONE;
            skip = typeAt(tokens, index + ONE) == TOKEN_KEYWORD_OUTER ? TWO : ONE;
        }

        if (typeAt(tokens, index + skip) != TOKEN_KEYWORD_JOIN)
            break;

        index      += skip + ONE;
        expectOn    = ONE;
        expectOuter = left;
    }

    snprintf(out->table, NAME_LEN, "%s", out->tables[ZERO]);

    if (typeAt(tokens, index) == TOKEN_KEYWORD_WHERE) {
        index++;

        int errorCode = addCondition(tokens, &index, &out->where, &whereRoot);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    if (whereRoot >= ZERO) {
        out->where.root    = whereRoot;
        out->where.present = ONE;
    }

    if (typeAt(tokens, index) == TOKEN_KEYWORD_GROUP) {
        index++;
        if (typeAt(tokens, index++) != TOKEN_KEYWORD_BY)
            return ERROR_SYNTAX_EXPECTED_BY;

        for (;;) {
            if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
                return ERROR_SYNTAX_EXPECTED_COLUMN;
            if (out->ngroup == MAX_COLS)
                return ERROR_SYNTAX_TOO_MANY_COLUMNS;

            snprintf(out->groupBy[out->ngroup++], NAME_LEN, "%s",
                     tokens->tokens[index++].value);

            if (typeAt(tokens, index) != TOKEN_COMMA)
                break;
            index++;
        }
    }


    if (typeAt(tokens, index) == TOKEN_KEYWORD_HAVING) {
        int errorCode = parseHaving(tokens, &index, &out->having);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    if (typeAt(tokens, index) == TOKEN_KEYWORD_ORDER) {
        index++;
        if (typeAt(tokens, index++) != TOKEN_KEYWORD_BY)
            return ERROR_SYNTAX_EXPECTED_BY;

        for (;;) {
            if (out->norder == MAX_COLS)
                return ERROR_SYNTAX_TOO_MANY_COLUMNS;

            /* Terms name output columns, so "order by count(*)" is spelled the
               same way the select item was - parse it as one and keep the label
               it produces, exactly as HAVING does. */
            SelectItem item;
            int errorCode = parseSelectItem(tokens, &index, &out->exprs, &item);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            OrderTerm* term = &out->order[out->norder++];
            snprintf(term->column, NAME_LEN, "%s", item.label);
            term->descending = ZERO;

            if (typeAt(tokens, index) == TOKEN_KEYWORD_DESC) {
                term->descending = ONE;
                index++;
            }
            else if (typeAt(tokens, index) == TOKEN_KEYWORD_ASC) {
                index++;
            }

            if (typeAt(tokens, index) != TOKEN_COMMA)
                break;
            index++;
        }
    }

    if (typeAt(tokens, index) == TOKEN_KEYWORD_LIMIT) {
        index++;

        if (typeAt(tokens, index) != TOKEN_NUMBER)
            return ERROR_SYNTAX_EXPECTED_VALUE;

        errno = ZERO;
        long long rows = strtoll(tokens->tokens[index++].value, NULL, 10);
        if (errno == ERANGE || rows > INT_MAX)
            return ERROR_VALUE_OUT_OF_RANGE;

        out->limit = (int)rows;
    }

    return endOfStatement(tokens, index);
}

/*
 * use <database>
 */
static int parseUse(const TokenList* tokens, char* name)
{
    int index = ONE;                                    /* USE is token 0 */

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(name, NAME_LEN, "%s", tokens->tokens[index++].value);

    return endOfStatement(tokens, index);
}

/*
 * create database <name>
 */
static int parseCreateDatabase(const TokenList* tokens, char* name)
{
    int index = TWO;                                    /* skip CREATE DATABASE */

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(name, NAME_LEN, "%s", tokens->tokens[index++].value);

    return endOfStatement(tokens, index);
}

/*
 * drop table <name> | drop index <name>
 */
static int parseDrop(const TokenList* tokens, StatementType* type, char* name)
{
    int index = ONE;                                    /* DROP is token 0 */

    if (typeAt(tokens, index) == TOKEN_KEYWORD_TABLE)
        *type = STMT_DROP_TABLE;
    else if (typeAt(tokens, index) == TOKEN_KEYWORD_INDEX)
        *type = STMT_DROP_INDEX;
    else if (typeAt(tokens, index) == TOKEN_KEYWORD_DATABASE)
        *type = STMT_DROP_DATABASE;
    else
        return ERROR_SYNTAX_INVALID_STATEMENT;
    index++;

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(name, NAME_LEN, "%s", tokens->tokens[index++].value);

    return endOfStatement(tokens, index);
}

/*
 * insert into <table> [( <column> [, <column>]* )] values ( <literal> [, ...] )
 */
static int parseInsert(const TokenList* tokens, InsertStatement* out)
{
    int index = ONE;                                    /* INSERT is token 0 */

    if (typeAt(tokens, index++) != TOKEN_KEYWORD_INTO)
        return ERROR_SYNTAX_INVALID_STATEMENT;

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(out->table, NAME_LEN, "%s", tokens->tokens[index++].value);

    out->ncolumns = ZERO;

    /* A parenthesis here opens a column list; VALUES opens the values. */
    if (typeAt(tokens, index) == TOKEN_LPAREN) {
        index++;

        for (;;) {
            if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
                return ERROR_SYNTAX_EXPECTED_COLUMN;
            if (out->ncolumns == MAX_COLS)
                return ERROR_SYNTAX_TOO_MANY_COLUMNS;

            snprintf(out->columns[out->ncolumns++], NAME_LEN, "%s",
                     tokens->tokens[index++].value);

            if (typeAt(tokens, index) != TOKEN_COMMA)
                break;
            index++;
        }

        if (typeAt(tokens, index++) != TOKEN_RPAREN)
            return ERROR_SYNTAX_EXPECTED_PARENTHESES;
    }

    if (typeAt(tokens, index++) != TOKEN_KEYWORD_VALUES)
        return ERROR_SYNTAX_EXPECTED_VALUES;

    if (typeAt(tokens, index++) != TOKEN_LPAREN)
        return ERROR_SYNTAX_EXPECTED_PARENTHESES;

    out->nvalues = ZERO;
    for (;;) {
        if (out->nvalues == MAX_COLS)
            return ERROR_SYNTAX_TOO_MANY_COLUMNS;

        int errorCode = parseValue(tokens, &index, &out->values[out->nvalues]);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        out->nvalues++;

        if (typeAt(tokens, index) != TOKEN_COMMA)
            break;
        index++;
    }

    if (typeAt(tokens, index++) != TOKEN_RPAREN)
        return ERROR_SYNTAX_EXPECTED_PARENTHESES;

    return endOfStatement(tokens, index);
}

/*
 * create index <name> on <table> ( <column> )
 */
static int parseCreateIndex(const TokenList* tokens, CreateIndexStatement* out)
{
    int index = TWO;                                    /* skip CREATE INDEX */

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(out->name, NAME_LEN, "%s", tokens->tokens[index++].value);

    if (typeAt(tokens, index++) != TOKEN_KEYWORD_ON)
        return ERROR_SYNTAX_EXPECTED_ON;

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(out->table, NAME_LEN, "%s", tokens->tokens[index++].value);

    if (typeAt(tokens, index++) != TOKEN_LPAREN)
        return ERROR_SYNTAX_EXPECTED_PARENTHESES;

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_COLUMN;
    snprintf(out->column, NAME_LEN, "%s", tokens->tokens[index++].value);

    if (typeAt(tokens, index++) != TOKEN_RPAREN)
        return ERROR_SYNTAX_EXPECTED_PARENTHESES;

    return endOfStatement(tokens, index);
}

/*
 * delete from <table> [where ...]
 */
static int parseDelete(const TokenList* tokens, DeleteStatement* out)
{
    int index = ONE;                                    /* DELETE is token 0 */

    out->where.present = ZERO;

    if (typeAt(tokens, index++) != TOKEN_KEYWORD_FROM)
        return ERROR_SYNTAX_EXPECTED_FROM;

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(out->table, NAME_LEN, "%s", tokens->tokens[index++].value);

    if (typeAt(tokens, index) == TOKEN_KEYWORD_WHERE) {
        int errorCode = parseWhere(tokens, &index, &out->where);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    return endOfStatement(tokens, index);
}

/*
 * update <table> set <column> = <literal> [, <column> = <literal>]* [where ...]
 */
static int parseUpdate(const TokenList* tokens, UpdateStatement* out)
{
    int index = ONE;                                    /* UPDATE is token 0 */

    out->nsets         = ZERO;
    out->where.present = ZERO;
    exprInit(&out->exprs);
    exprInit(&out->where.exprs);

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(out->table, NAME_LEN, "%s", tokens->tokens[index++].value);

    if (typeAt(tokens, index++) != TOKEN_KEYWORD_SET)
        return ERROR_SYNTAX_EXPECTED_SET;

    for (;;) {
        if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
            return ERROR_SYNTAX_EXPECTED_COLUMN;
        if (out->nsets == MAX_COLS)
            return ERROR_SYNTAX_TOO_MANY_COLUMNS;

        Assignment* assignment = &out->sets[out->nsets];
        snprintf(assignment->column, NAME_LEN, "%s", tokens->tokens[index++].value);

        if (typeAt(tokens, index++) != TOKEN_OPERATOR_EQ)
            return ERROR_SYNTAX_EXPECTED_ASSIGNMENT;

        /* "set balance = balance * 1.1" is the point of this being an
           expression rather than a literal. */
        int errorCode = parseExpression(tokens, &index, &out->exprs,
                                        &assignment->expr);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        out->nsets++;

        if (typeAt(tokens, index) != TOKEN_COMMA)
            break;
        index++;
    }

    if (typeAt(tokens, index) == TOKEN_KEYWORD_WHERE) {
        int errorCode = parseWhere(tokens, &index, &out->where);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    return endOfStatement(tokens, index);
}

/*
 * vacuum <table>
 */
static int parseVacuum(const TokenList* tokens, char* table)
{
    int index = ONE;                                    /* VACUUM is token 0 */

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(table, NAME_LEN, "%s", tokens->tokens[index++].value);

    return endOfStatement(tokens, index);
}

/*
 * alter table <t> add [column] <c> <type> [constraints]
 * alter table <t> drop column <c>
 * alter table <t> rename column <c> to <new>
 * alter table <t> rename to <new>
 *
 * The constraints a new column may carry are DEFAULT and NOT NULL. UNIQUE,
 * PRIMARY KEY and CHECK are refused rather than half-honoured: each of them
 * makes a claim about rows that already exist, and the rows are not looked at
 * here. That refusal lives in the semantic stage, which can see the table.
 */
static int parseAlter(const TokenList* tokens, AlterStatement* out)
{
    int index = ONE;                                    /* ALTER is token 0 */

    if (typeAt(tokens, index++) != TOKEN_KEYWORD_TABLE)
        return ERROR_SYNTAX_EXPECTED_TABLE;

    if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
        return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
    snprintf(out->table, NAME_LEN, "%s", tokens->tokens[index++].value);

    memset(&out->column, ZERO, sizeof out->column);
    out->name[ZERO]    = '\0';
    out->newName[ZERO] = '\0';

    switch (typeAt(tokens, index++)) {
    case TOKEN_KEYWORD_ADD: {
        out->action = ALTER_ADD_COLUMN;

        /* COLUMN is noise here, as it is in every dialect that accepts it */
        if (typeAt(tokens, index) == TOKEN_KEYWORD_COLUMN)
            index++;

        if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
            return ERROR_SYNTAX_EXPECTED_COLUMN;
        snprintf(out->column.name, NAME_LEN, "%s", tokens->tokens[index++].value);

        int errorCode = parseColumnType(tokens, &index, &out->column);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        /* A CHECK written here would have to be ANDed into a tree the table
           already owns, so the scratch one is passed and its root inspected:
           anything that landed in it is a CHECK, and CHECK is not supported. */
        Condition scratch = { ZERO };
        int       root    = -ONE;

        scratch.count = ZERO;
        errorCode = parseColumnConstraints(tokens, &index, &out->column,
                                           &scratch, &root);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        if (root != -ONE)
            return ERROR_SEMANTIC_ALTER_UNSUPPORTED;

        break;
    }

    case TOKEN_KEYWORD_DROP:
        out->action = ALTER_DROP_COLUMN;

        if (typeAt(tokens, index) == TOKEN_KEYWORD_COLUMN)
            index++;

        if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
            return ERROR_SYNTAX_EXPECTED_COLUMN;
        snprintf(out->name, NAME_LEN, "%s", tokens->tokens[index++].value);
        break;

    case TOKEN_KEYWORD_RENAME:
        /* "rename to x" renames the table; "rename column c to x" renames a
           column. The word COLUMN is what separates them, so unlike above it
           is not optional. */
        if (typeAt(tokens, index) == TOKEN_KEYWORD_TO) {
            index++;
            out->action = ALTER_RENAME_TABLE;

            if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
                return ERROR_SYNTAX_EXPECTED_TABLE_NAME;
            snprintf(out->newName, NAME_LEN, "%s", tokens->tokens[index++].value);
            break;
        }

        if (typeAt(tokens, index++) != TOKEN_KEYWORD_COLUMN)
            return ERROR_SYNTAX_EXPECTED_COLUMN;

        out->action = ALTER_RENAME_COLUMN;

        if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
            return ERROR_SYNTAX_EXPECTED_COLUMN;
        snprintf(out->name, NAME_LEN, "%s", tokens->tokens[index++].value);

        if (typeAt(tokens, index++) != TOKEN_KEYWORD_TO)
            return ERROR_SYNTAX_EXPECTED_TO;

        if (typeAt(tokens, index) != TOKEN_IDENTIFIER)
            return ERROR_SYNTAX_EXPECTED_COLUMN;
        snprintf(out->newName, NAME_LEN, "%s", tokens->tokens[index++].value);
        break;

    default:
        return ERROR_SYNTAX_INVALID_STATEMENT;
    }

    return endOfStatement(tokens, index);
}

/*
 * Dispatches on the first token, exactly like parseStatement in the compiler.
 */
int parseStatement(const TokenList* tokens, Statement* out)
{
    if (tokens->count == ZERO)
        return SUCCESS_CODE;

    switch (tokens->tokens[ZERO].type) {
    case TOKEN_KEYWORD_SELECT:
        out->type = STMT_SELECT;
        return parseSelect(tokens, &out->u.select);

    case TOKEN_KEYWORD_CREATE:
        if (typeAt(tokens, ONE) == TOKEN_KEYWORD_DATABASE) {
            out->type = STMT_CREATE_DATABASE;
            return parseCreateDatabase(tokens, out->u.databaseName);
        }
        if (typeAt(tokens, ONE) == TOKEN_KEYWORD_INDEX) {
            out->type = STMT_CREATE_INDEX;
            return parseCreateIndex(tokens, &out->u.createIndex);
        }
        out->type = STMT_CREATE_TABLE;
        return parseCreateTable(tokens, &out->u.create);

    case TOKEN_KEYWORD_INSERT:
        out->type = STMT_INSERT;
        return parseInsert(tokens, &out->u.insert);

    case TOKEN_KEYWORD_DELETE:
        out->type = STMT_DELETE;
        return parseDelete(tokens, &out->u.del);

    case TOKEN_KEYWORD_UPDATE:
        out->type = STMT_UPDATE;
        return parseUpdate(tokens, &out->u.update);

    case TOKEN_KEYWORD_VACUUM:
        out->type = STMT_VACUUM;
        return parseVacuum(tokens, out->u.vacuumTable);

    case TOKEN_KEYWORD_USE:
        out->type = STMT_USE_DATABASE;
        return parseUse(tokens, out->u.databaseName);

    case TOKEN_KEYWORD_BEGIN:
        out->type = STMT_BEGIN;
        return endOfStatement(tokens, ONE);

    case TOKEN_KEYWORD_COMMIT:
        out->type = STMT_COMMIT;
        return endOfStatement(tokens, ONE);

    case TOKEN_KEYWORD_ROLLBACK:
        out->type = STMT_ROLLBACK;
        return endOfStatement(tokens, ONE);

    case TOKEN_KEYWORD_ALTER:
        out->type = STMT_ALTER_TABLE;
        return parseAlter(tokens, &out->u.alter);

    case TOKEN_KEYWORD_DROP:
        return parseDrop(tokens, &out->type, out->u.dropName);

    default:
        return ERROR_SYNTAX_INVALID_STATEMENT;
    }
}
