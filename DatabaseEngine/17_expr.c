#include "sql_common.h"
#include <limits.h>

/*
 * Arithmetic over columns and literals: what a query means by "price * qty".
 *
 * This sits between the semantic stage and the executor because both need it
 * and neither owns it. The parser builds the tree, `exprResolve` binds each
 * column to a position and fits each literal to the type it will meet, and
 * `exprEvaluate` turns a row into a value.
 *
 * The tree is a flat pool with indices for edges, like a Condition, so it
 * copies by value, allocates nothing, and is written to disk as itself when a
 * CHECK carries one.
 *
 * Types follow one rule: an expression is a float if any part of it is, and an
 * int otherwise. Text and dates are not arithmetic - a date is a day number,
 * but "a date plus three" meaning three days is a decision this engine has not
 * made, so it is refused rather than guessed at.
 */

void exprInit(ExprPool* pool)
{
    pool->count = ZERO;
}

int exprNew(ExprPool* pool, ExprKind kind, int* node)
{
    if (pool->count == MAX_EXPR_NODES)
        return ERROR_SYNTAX_EXPRESSION_TOO_COMPLEX;

    *node = pool->count++;

    ExprNode* fresh = &pool->nodes[*node];

    fresh->kind      = kind;
    fresh->op        = ARITH_ADD;
    fresh->left      = -1;
    fresh->right     = -1;
    fresh->slot      = -1;
    fresh->type      = TYPE_INT;
    fresh->column[ZERO] = '\0';
    fresh->literal   = (Value){ ZERO };

    return SUCCESS_CODE;
}

/*
 * The simple shapes, which several stages still care about: an index can only
 * seek to a constant, a hash join can only key on a column, and a header reads
 * better as the column's own name. Asking beats keeping a second copy of the
 * operand in the predicate.
 */
int exprIsColumn(const ExprPool* pool, int node)
{
    return node >= ZERO && node < pool->count
        && pool->nodes[node].kind == EXPR_COLUMN;
}

int exprIsLiteral(const ExprPool* pool, int node)
{
    return node >= ZERO && node < pool->count
        && pool->nodes[node].kind == EXPR_LITERAL;
}

const char* exprColumn(const ExprPool* pool, int node)
{
    return exprIsColumn(pool, node) ? pool->nodes[node].column : "";
}

Value* exprLiteral(ExprPool* pool, int node)
{
    return exprIsLiteral(pool, node) ? &pool->nodes[node].literal : NULL;
}

/* ---------- naming ---------- */

static char arithSymbol(ArithOp op)
{
    switch (op) {
    case ARITH_ADD: return '+';
    case ARITH_SUB: return '-';
    case ARITH_MUL: return '*';
    case ARITH_DIV: return '/';
    default:        return '%';
    }
}

/*
 * What the column is called in the output. Postgres answers "?column?" for
 * anything that is not a plain column; spelling the expression back out is more
 * use than that, and it is also what ORDER BY and HAVING match against, so
 * "order by a*2" finds the item that produced it.
 */
void exprLabel(const ExprPool* pool, int node, char* out, size_t size)
{
    if (size == ZERO)
        return;

    out[ZERO] = '\0';

    if (node < ZERO || node >= pool->count)
        return;

    const ExprNode* current = &pool->nodes[node];

    switch (current->kind) {
    case EXPR_COLUMN:
        snprintf(out, size, "%s", current->column);
        return;

    case EXPR_LITERAL: {
        const Value* value = &current->literal;

        if (value->isNull)
            snprintf(out, size, "NULL");
        else if (value->type == TYPE_INT)
            snprintf(out, size, "%d", value->intValue);
        else if (value->type == TYPE_FLOAT)
            snprintf(out, size, "%g", value->floatValue);
        else if (value->type == TYPE_DATE) {
            char text[11];

            dateToText(value->intValue, text);
            snprintf(out, size, "'%s'", text);
        }
        else
            snprintf(out, size, "'%.*s'", value->textLength, valueText(value));
        return;
    }

    case EXPR_NEGATE: {
        char inner[NAME_LEN];

        exprLabel(pool, current->left, inner, sizeof inner);
        snprintf(out, size, "-%s", inner);
        return;
    }

    default: {
        char left[NAME_LEN];
        char right[NAME_LEN];

        exprLabel(pool, current->left, left, sizeof left);
        exprLabel(pool, current->right, right, sizeof right);

        /* Brackets wherever a child is itself a sum or a product, so the name
           reads back as the expression it stands for rather than as another
           one with the same words in it. */
        int wrapLeft  = pool->nodes[current->left].kind == EXPR_BINARY;
        int wrapRight = current->right >= ZERO
                        && pool->nodes[current->right].kind == EXPR_BINARY;

        snprintf(out, size, "%s%s%s%c%s%s%s",
                 wrapLeft ? "(" : "", left, wrapLeft ? ")" : "",
                 arithSymbol(current->op),
                 wrapRight ? "(" : "", right, wrapRight ? ")" : "");
        return;
    }
    }
}

/* ---------- binding to a table ---------- */

/*
 * Resolves every column in the tree to its position, and settles what a literal
 * standing beside a column must be: "where day > '2024-01-01'" only knows the
 * text is a date because the column says so.
 */
/*
 * The name that last failed to resolve. Kept so that a caller which knows more
 * about the context can say something better than "no such column" - the one
 * that does is the subquery check, which can see whether the name would have
 * resolved in the query outside and report a correlated subquery instead.
 */
static char unresolved[NAME_LEN];

const char* exprUnresolvedColumn(void)
{
    return unresolved;
}

int exprResolve(const CatalogNode* table, ExprPool* pool, int node)
{
    if (node < ZERO || node >= pool->count)
        return ERROR_SEMANTIC_COLUMN_NOT_FOUND;

    ExprNode* current = &pool->nodes[node];

    if (current->kind == EXPR_COLUMN) {
        current->slot = findColumn(table, current->column);

        if (current->slot == COLUMN_AMBIGUOUS)
            return ERROR_SEMANTIC_AMBIGUOUS_COLUMN;
        if (current->slot < ZERO) {
            snprintf(unresolved, NAME_LEN, "%s", current->column);
            return ERROR_SEMANTIC_COLUMN_NOT_FOUND;
        }

        current->type = table->cols[current->slot].type;
        return SUCCESS_CODE;
    }

    if (current->kind == EXPR_LITERAL) {
        current->type = current->literal.type;
        return SUCCESS_CODE;
    }

    int errorCode = exprResolve(table, pool, current->left);

    if (errorCode != SUCCESS_CODE || current->kind == EXPR_NEGATE)
        return errorCode;

    return exprResolve(table, pool, current->right);
}

/*
 * The type the tree produces, or a mismatch when it does not produce one.
 * Called after resolving, so a column knows its own slot - but the table is not
 * needed here because resolving already copied the type onto the node.
 */
int exprType(const ExprPool* pool, int node, ColType* out)
{
    if (node < ZERO || node >= pool->count)
        return ERROR_SEMANTIC_COLUMN_NOT_FOUND;

    const ExprNode* current = &pool->nodes[node];

    if (current->kind == EXPR_COLUMN || current->kind == EXPR_LITERAL) {
        *out = current->type;               /* stamped by exprResolve */
        return SUCCESS_CODE;
    }

    ColType left;
    int     errorCode = exprType(pool, current->left, &left);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    if (current->kind == EXPR_NEGATE) {
        if (left != TYPE_INT && left != TYPE_FLOAT)
            return ERROR_SEMANTIC_TYPE_MISMATCH;

        *out = left;
        return SUCCESS_CODE;
    }

    ColType right;

    errorCode = exprType(pool, current->right, &right);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /* Arithmetic is for numbers. Text has no sum, and a date is a day count
       whose arithmetic would have to mean something this engine has not
       decided - so both are refused rather than quietly reinterpreted. */
    if ((left != TYPE_INT && left != TYPE_FLOAT)
        || (right != TYPE_INT && right != TYPE_FLOAT))
        return ERROR_SEMANTIC_TYPE_MISMATCH;

    *out = (left == TYPE_FLOAT || right == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT;
    return SUCCESS_CODE;
}

/*
 * The deepest column the tree reads. A scan decodes a record that far and no
 * further, so an expression over the first column of a wide row still stops
 * after one field.
 */
int exprDeepest(const ExprPool* pool, int node)
{
    if (node < ZERO || node >= pool->count)
        return -1;

    const ExprNode* current = &pool->nodes[node];

    if (current->kind == EXPR_COLUMN)
        return current->slot;
    if (current->kind == EXPR_LITERAL)
        return -1;

    int deepest = exprDeepest(pool, current->left);

    if (current->kind == EXPR_NEGATE)
        return deepest;

    int right = exprDeepest(pool, current->right);

    return right > deepest ? right : deepest;
}

/* ---------- evaluating ---------- */

static int applyInts(ArithOp op, int left, int right, Value* out)
{
    long long result;

    switch (op) {
    case ARITH_ADD: result = (long long)left + right; break;
    case ARITH_SUB: result = (long long)left - right; break;
    case ARITH_MUL: result = (long long)left * right; break;

    case ARITH_DIV:
        if (right == ZERO)
            return ERROR_EXEC_DIVIDE_BY_ZERO;

        /* INT_MIN / -1 is the one division that overflows */
        if (left == INT_MIN && right == -1)
            return ERROR_VALUE_OUT_OF_RANGE;

        result = left / right;                  /* integer division, as SQL has */
        break;

    default:
        if (right == ZERO)
            return ERROR_EXEC_DIVIDE_BY_ZERO;
        if (left == INT_MIN && right == -1)
            return ERROR_VALUE_OUT_OF_RANGE;

        result = left % right;
        break;
    }

    /* Computed wide and checked once, because a sum of two ints does not fit
       in an int and finding that out afterwards is too late. */
    if (result < INT_MIN || result > INT_MAX)
        return ERROR_VALUE_OUT_OF_RANGE;

    out->type       = TYPE_INT;
    out->isNull     = ZERO;
    out->intValue   = (int)result;
    out->text       = NULL;
    out->textLength = ZERO;
    return SUCCESS_CODE;
}

static int applyReals(ArithOp op, double left, double right, Value* out)
{
    double result;

    switch (op) {
    case ARITH_ADD: result = left + right; break;
    case ARITH_SUB: result = left - right; break;
    case ARITH_MUL: result = left * right; break;

    case ARITH_DIV:
        if (right == 0.0)
            return ERROR_EXEC_DIVIDE_BY_ZERO;
        result = left / right;
        break;

    default:
        return ERROR_SEMANTIC_TYPE_MISMATCH;    /* no modulo on reals */
    }

    setFloat(out, result);
    return SUCCESS_CODE;
}

static double asReal(const Value* value)
{
    return value->type == TYPE_FLOAT ? value->floatValue : (double)value->intValue;
}

/*
 * One row through the tree.
 *
 * NULL is contagious: anything computed from an unknown is unknown, which is
 * what stops "price * qty" inventing a number for a row that has neither. The
 * comparison above it then sees a NULL operand and answers UNKNOWN, exactly as
 * a bare NULL column would.
 */
int exprEvaluate(const ExprPool* pool, int node, const Row* row, Value* out)
{
    if (node < ZERO || node >= pool->count)
        return ERROR_SEMANTIC_COLUMN_NOT_FOUND;

    const ExprNode* current = &pool->nodes[node];

    if (current->kind == EXPR_COLUMN) {
        if (current->slot < ZERO || current->slot >= MAX_COLS)
            return ERROR_SEMANTIC_COLUMN_NOT_FOUND;

        *out = row->values[current->slot];
        return SUCCESS_CODE;
    }

    if (current->kind == EXPR_LITERAL) {
        *out = current->literal;
        return SUCCESS_CODE;
    }

    Value left;
    int   errorCode = exprEvaluate(pool, current->left, row, &left);

    if (errorCode != SUCCESS_CODE)
        return errorCode;

    if (current->kind == EXPR_NEGATE) {
        if (left.isNull) {
            *out = left;
            return SUCCESS_CODE;
        }

        if (left.type == TYPE_FLOAT) {
            setFloat(out, -left.floatValue);
            return SUCCESS_CODE;
        }

        if (left.type != TYPE_INT)
            return ERROR_SEMANTIC_TYPE_MISMATCH;

        return applyInts(ARITH_SUB, ZERO, left.intValue, out);
    }

    Value right;

    errorCode = exprEvaluate(pool, current->right, row, &right);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    if (left.isNull || right.isNull) {
        setNull(out, left.isNull ? left.type : right.type);
        return SUCCESS_CODE;
    }

    if ((left.type != TYPE_INT && left.type != TYPE_FLOAT)
        || (right.type != TYPE_INT && right.type != TYPE_FLOAT))
        return ERROR_SEMANTIC_TYPE_MISMATCH;

    if (left.type == TYPE_FLOAT || right.type == TYPE_FLOAT)
        return applyReals(current->op, asReal(&left), asReal(&right), out);

    return applyInts(current->op, left.intValue, right.intValue, out);
}
