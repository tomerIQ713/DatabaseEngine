#include "sql_common.h"

/*
 * Is this column one of the GROUP BY columns?
 */
static int isGrouped(const SelectStatement* select, const char* column)
{
    for (int i = ZERO; i < select->ngroup; i++)
        if (_stricmp(select->groupBy[i], column) == ZERO)
            return ONE;
    return ZERO;
}

/*
 * findColumn folds "no such column" and "more than one table has it" into one
 * negative return; only the message differs.
 */
static int columnError(int slot)
{
    return slot == COLUMN_AMBIGUOUS
         ? ERROR_SEMANTIC_AMBIGUOUS_COLUMN : ERROR_SEMANTIC_COLUMN_NOT_FOUND;
}

/*
 * Fits a literal to the column it is about to meet.
 *
 * The lexer cannot know that '2024-05-01' is a date or that 3 is standing in
 * for 3.0 - only the column says so - and this is the last stage that still has
 * both the literal and the column in hand. Everything below it therefore sees
 * values that already carry the column's own type, and no execution path has to
 * know that a date was ever written as text.
 *
 * A varchar's length is not checked here: a comparison against a too-long
 * string is a comparison that matches nothing, not an error. Storing one is,
 * and that is checked where the row is stored.
 */
int coerceLiteral(Value* value, ColType type)
{
    if (value->isNull) {                    /* NULL takes the column's type */
        value->type = type;
        return SUCCESS_CODE;
    }

    if (value->type == type)
        return SUCCESS_CODE;

    if (type == TYPE_FLOAT && value->type == TYPE_INT) {
        setFloat(value, (double)value->intValue);
        return SUCCESS_CODE;
    }

    if (type == TYPE_DATE && value->type == TYPE_TEXT) {
        int days;
        int errorCode = textToDate(value->text, value->textLength, &days);

        if (errorCode != SUCCESS_CODE)
            return errorCode;

        value->type       = TYPE_DATE;
        value->intValue   = days;
        value->text       = NULL;
        value->textLength = ZERO;
        return SUCCESS_CODE;
    }

    return ERROR_SEMANTIC_TYPE_MISMATCH;
}

/*
 * Walks the WHERE tree, checking every comparison against the table.
 */
static int checkCondition(const CatalogNode* table, Condition* cond, int node)
{
    ConditionNode* current = &cond->nodes[node];

    if (current->kind == COND_COMPARE) {
        Predicate* compare = &current->compare;
        ColType    left;

        int errorCode = exprResolve(table, &cond->exprs, compare->left);

        if (errorCode == SUCCESS_CODE)
            errorCode = exprType(&cond->exprs, compare->left, &left);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        /* IS NULL asks about a value's presence, whatever type it has */
        if (compare->op == OP_IS_NULL || compare->op == OP_IS_NOT_NULL)
            return SUCCESS_CODE;

        /* LIKE is a pattern over stored text, so it says nothing about a
           number or a day count and the left side has to be text. */
        if (compare->op == OP_LIKE || compare->op == OP_NOT_LIKE)
            return left == TYPE_TEXT ? SUCCESS_CODE : ERROR_SEMANTIC_TYPE_MISMATCH;

        errorCode = exprResolve(table, &cond->exprs, compare->right);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        ColType right;

        errorCode = exprType(&cond->exprs, compare->right, &right);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        /*
         * A bare literal takes the type of whatever it is being compared with,
         * which is how '2024-05-01' becomes a date and 3 becomes 3.0. Only a
         * literal bends: two expressions have to agree on their own.
         */
        if (left != right) {
            Value* literal = exprLiteral(&cond->exprs, compare->right);

            if (literal != NULL) {
                errorCode = coerceLiteral(literal, left);
                right     = left;
            }
            else if ((literal = exprLiteral(&cond->exprs, compare->left)) != NULL) {
                errorCode = coerceLiteral(literal, right);
                left      = right;
            }

            if (errorCode != SUCCESS_CODE)
                return errorCode;

            /* the node remembers what its literal became */
            exprResolve(table, &cond->exprs, compare->left);
            exprResolve(table, &cond->exprs, compare->right);
        }

        return left == right ? SUCCESS_CODE : ERROR_SEMANTIC_TYPE_MISMATCH;
    }

    if (current->kind == COND_NOT)
        return checkCondition(table, cond, current->left);

    int errorCode = checkCondition(table, cond, current->left);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    return checkCondition(table, cond, current->right);
}

static int checkWhere(const CatalogNode* table, Condition* cond)
{
    return cond->present ? checkCondition(table, cond, cond->root) : SUCCESS_CODE;
}

static int checkSelect(SelectStatement* select)
{
    CatalogNode        joined;
    const CatalogNode* table;

    /* A join is checked against the flattened schema, so everything below this
       point is the same code the single-table case runs. */
    if (joinSchemaNeeded(select)) {
        /* through a const view: the statement is mutable here so that literals
           can be fitted to their columns, but the schema builder only reads */
        const SelectStatement* reading = select;

        int errorCode = buildJoinSchema(reading->tables, reading->aliases,
                                        reading->ntables, &joined);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
        table = &joined;
    }
    else {
        table = findTable(select->table);
        if (table == NULL)
            return ERROR_SEMANTIC_TABLE_NOT_FOUND;
    }

    int errorCode = checkWhere(table, &select->where);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    for (int i = ZERO; i < select->ngroup; i++) {
        int slot = findColumn(table, select->groupBy[i]);
        if (slot < ZERO)
            return columnError(slot);
    }

    if (select->selectAll) {
        /* SELECT * cannot be grouped: there is no column list to check against */
        if (select->ngroup > ZERO)
            return ERROR_SEMANTIC_NOT_GROUPED;
        return select->having.present
             ? ERROR_SEMANTIC_HAVING_WITHOUT_GROUP : SUCCESS_CODE;
    }

    int hasAggregate = ZERO;
    for (int i = ZERO; i < select->nitems; i++)
        if (select->items[i].aggregate != AGG_NONE)
            hasAggregate = ONE;

    /* HAVING filters groups, so there have to be groups to filter */
    if (select->having.present && select->ngroup == ZERO && !hasAggregate)
        return ERROR_SEMANTIC_HAVING_WITHOUT_GROUP;

    for (int i = ZERO; i < select->nitems; i++) {
        SelectItem* item = &select->items[i];

        if (item->star)                             /* count(*) needs no column */
            continue;

        int errorCode = exprResolve(table, &select->exprs, item->expr);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        ColType type;

        errorCode = exprType(&select->exprs, item->expr, &type);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        /* sum and avg are arithmetic, so what they add up has to be a number */
        if ((item->aggregate == AGG_SUM || item->aggregate == AGG_AVG)
            && type != TYPE_INT && type != TYPE_FLOAT)
            return ERROR_SEMANTIC_TYPE_MISMATCH;

        if (item->aggregate != AGG_NONE
            || (select->ngroup == ZERO && !hasAggregate))
            continue;

        /*
         * Beside an aggregate, an item has to be one value per group. A
         * grouped column is; an expression over ungrouped columns is one value
         * per row, and there is no single answer to print for the group.
         */
        if (!exprIsColumn(&select->exprs, item->expr)
            || !isGrouped(select, exprColumn(&select->exprs, item->expr)))
            return ERROR_SEMANTIC_NOT_GROUPED;
    }

    return SUCCESS_CODE;
}

/*
 * Checks the parsed statement against the catalog.
 */
int semanticCheck(Statement* statement)
{
    switch (statement->type) {

    case STMT_CREATE_TABLE: {
        CreateStatement* create = &statement->u.create;

        if (findTable(create->table) != NULL)
            return ERROR_SEMANTIC_TABLE_EXISTS;

        for (int i = ZERO; i < create->ncols; i++)
            for (int j = i + ONE; j < create->ncols; j++)
                if (_stricmp(create->cols[i].name, create->cols[j].name) == ZERO)
                    return ERROR_SEMANTIC_DUPLICATE_COLUMN;

        /* A default is a literal that will be stored in the column every time
           the column is left out, so it has to fit the column now rather than
           on the first insert that relies on it. */
        for (int i = ZERO; i < create->ncols; i++) {
            if (!create->cols[i].hasDefault)
                continue;

            int errorCode = coerceLiteral(&create->cols[i].defaultValue,
                                          create->cols[i].type);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            if (create->cols[i].defaultValue.isNull
                && (create->cols[i].flags & COL_NOT_NULL))
                return ERROR_EXEC_NOT_NULL;
        }

        /* The CHECK names columns of a table that does not exist yet, so it is
           resolved against the one being described. */
        if (create->check.present) {
            CatalogNode proposed;

            snprintf(proposed.table, NAME_LEN, "%s", create->table);
            memcpy(proposed.cols, create->cols,
                   (size_t)create->ncols * sizeof(Column));
            proposed.ncols = create->ncols;
            proposed.check = NULL;
            proposed.next  = NULL;

            return checkCondition(&proposed, &create->check, create->check.root);
        }

        return SUCCESS_CODE;
    }

    case STMT_CREATE_INDEX: {
        const CreateIndexStatement* create = &statement->u.createIndex;

        if (findIndexByName(create->name) != NULL)
            return ERROR_SEMANTIC_INDEX_EXISTS;

        const CatalogNode* table = findTable(create->table);
        if (table == NULL)
            return ERROR_SEMANTIC_TABLE_NOT_FOUND;

        if (findColumn(table, create->column) < ZERO)
            return ERROR_SEMANTIC_COLUMN_NOT_FOUND;

        return SUCCESS_CODE;
    }

    case STMT_CREATE_DATABASE:
        return findDatabase(statement->u.databaseName) >= ZERO
             ? ERROR_SEMANTIC_DATABASE_EXISTS : SUCCESS_CODE;

    case STMT_USE_DATABASE:
        return findDatabase(statement->u.databaseName) >= ZERO
             ? SUCCESS_CODE : ERROR_SEMANTIC_DATABASE_NOT_FOUND;

    case STMT_DROP_DATABASE: {
        int id = findDatabase(statement->u.dropName);

        if (id < ZERO)
            return ERROR_SEMANTIC_DATABASE_NOT_FOUND;
        if (id == ZERO)
            return ERROR_SEMANTIC_CANNOT_DROP_DEFAULT;

        return SUCCESS_CODE;
    }

    case STMT_DROP_TABLE:
        return findTable(statement->u.dropName) != NULL
             ? SUCCESS_CODE : ERROR_SEMANTIC_TABLE_NOT_FOUND;

    case STMT_DROP_INDEX:
        return findIndexByName(statement->u.dropName) != NULL
             ? SUCCESS_CODE : ERROR_SEMANTIC_INDEX_NOT_FOUND;

    case STMT_VACUUM:
        return findTable(statement->u.vacuumTable) != NULL
             ? SUCCESS_CODE : ERROR_SEMANTIC_TABLE_NOT_FOUND;

    case STMT_SELECT:
        return checkSelect(&statement->u.select);

    case STMT_DELETE: {
        DeleteStatement* del = &statement->u.del;

        const CatalogNode* table = findTable(del->table);
        if (table == NULL)
            return ERROR_SEMANTIC_TABLE_NOT_FOUND;

        return checkWhere(table, &del->where);
    }

    case STMT_UPDATE: {
        UpdateStatement* update = &statement->u.update;

        const CatalogNode* table = findTable(update->table);
        if (table == NULL)
            return ERROR_SEMANTIC_TABLE_NOT_FOUND;

        for (int i = ZERO; i < update->nsets; i++) {
            int slot = findColumn(table, update->sets[i].column);
            if (slot < ZERO)
                return ERROR_SEMANTIC_COLUMN_NOT_FOUND;

            int errorCode = exprResolve(table, &update->exprs,
                                        update->sets[i].expr);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            ColType type;

            errorCode = exprType(&update->exprs, update->sets[i].expr, &type);
            if (errorCode != SUCCESS_CODE)
                return errorCode;

            /* A literal bends to the column, the way it does in a comparison;
               a computed value has to already be what the column holds. */
            if (type != table->cols[slot].type) {
                Value* literal = exprLiteral(&update->exprs, update->sets[i].expr);

                if (literal == NULL)
                    return ERROR_SEMANTIC_TYPE_MISMATCH;

                errorCode = coerceLiteral(literal, table->cols[slot].type);
                if (errorCode != SUCCESS_CODE)
                    return errorCode;

                exprResolve(table, &update->exprs, update->sets[i].expr);
            }

            /* assigning the same column twice has no defined winner */
            for (int j = i + ONE; j < update->nsets; j++)
                if (_stricmp(update->sets[i].column, update->sets[j].column) == ZERO)
                    return ERROR_SEMANTIC_DUPLICATE_COLUMN;
        }

        return checkWhere(table, &update->where);
    }

    case STMT_INSERT: {
        InsertStatement* insert = &statement->u.insert;

        const CatalogNode* table = findTable(insert->table);
        if (table == NULL)
            return ERROR_SEMANTIC_TABLE_NOT_FOUND;

        /* Without a column list the values are positional, and there has to
           be exactly one for each column. */
        if (insert->ncolumns == ZERO) {
            if (insert->nvalues != table->ncols)
                return ERROR_SEMANTIC_COLUMN_COUNT;

            for (int i = ZERO; i < table->ncols; i++) {
                int errorCode = coerceLiteral(&insert->values[i],
                                              table->cols[i].type);
                if (errorCode != SUCCESS_CODE)
                    return errorCode;
            }

            return SUCCESS_CODE;
        }

        if (insert->nvalues != insert->ncolumns)
            return ERROR_SEMANTIC_COLUMN_COUNT;

        int listed[MAX_COLS] = { ZERO };

        for (int i = ZERO; i < insert->ncolumns; i++) {
            int slot = findColumn(table, insert->columns[i]);
            if (slot < ZERO)
                return columnError(slot);

            if (listed[slot])               /* two values for one column */
                return ERROR_SEMANTIC_DUPLICATE_COLUMN;
            listed[slot] = ONE;

            int errorCode = coerceLiteral(&insert->values[i],
                                          table->cols[slot].type);
            if (errorCode != SUCCESS_CODE)
                return errorCode;
        }

        /* Every column the list left out needs something to put there: its
           default, or NULL when it will take one. */
        for (int c = ZERO; c < table->ncols; c++)
            if (!listed[c] && !table->cols[c].hasDefault
                && (table->cols[c].flags & COL_NOT_NULL))
                return ERROR_SEMANTIC_MISSING_VALUE;

        return SUCCESS_CODE;
    }

    default:
        return SUCCESS_CODE;
    }
}
