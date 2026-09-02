#include "sql_common.h"
/*
 * Maps an error code to a human-readable message.
 */
const char* errorCodeToString(int errorCode)
{
    switch (errorCode) {
    case SUCCESS_CODE:                      return "ok";

    case ERROR_UNKNOWN_TOKEN:               return "unknown token";
    case ERROR_UNTERMINATED_STRING:         return "unterminated string literal";
    case ERROR_TOO_MANY_TOKENS:             return "statement too long";
    case ERROR_VALUE_OUT_OF_RANGE:          return "integer literal out of range";
    case ERROR_TOKEN_TOO_LONG:              return "a name or literal is too long";

    case ERROR_SYNTAX_INVALID_STATEMENT:    return "not a valid statement";
    case ERROR_SYNTAX_EXPECTED_FROM:        return "expected FROM";
    case ERROR_SYNTAX_EXPECTED_TABLE_NAME:  return "expected a table name";
    case ERROR_SYNTAX_EXPECTED_COLUMN:      return "expected a column name";
    case ERROR_SYNTAX_EXPECTED_TYPE:        return "expected a column type";
    case ERROR_SYNTAX_EXPECTED_PARENTHESES: return "unbalanced parentheses";
    case ERROR_SYNTAX_TRAILING_TOKENS:      return "unexpected tokens after statement";
    case ERROR_SYNTAX_TOO_MANY_COLUMNS:     return "too many columns";
    case ERROR_SYNTAX_EXPECTED_VALUES:      return "expected VALUES";
    case ERROR_SYNTAX_EXPECTED_VALUE:       return "expected a number or a quoted string";
    case ERROR_SYNTAX_EXPECTED_OPERATOR:    return "expected a comparison operator";
    case ERROR_SYNTAX_EXPECTED_BY:          return "expected BY after GROUP or ORDER";
    case ERROR_SYNTAX_UNKNOWN_FUNCTION:     return "unknown function";
    case ERROR_SYNTAX_EXPECTED_NULL:        return "expected NULL after IS";
    case ERROR_SYNTAX_EXPECTED_ON:          return "expected ON";
    case ERROR_SYNTAX_EXPECTED_SET:         return "expected SET";
    case ERROR_SYNTAX_EXPECTED_ASSIGNMENT:  return "expected = in a SET assignment";
    case ERROR_SYNTAX_CONDITION_TOO_COMPLEX:return "WHERE clause is too complex";
    case ERROR_SYNTAX_TOO_MANY_TABLES:      return "too many tables in FROM";
    case ERROR_SYNTAX_EXPECTED_KEY:         return "expected KEY after PRIMARY";
    case ERROR_SYNTAX_EXPECTED_SIZE:        return "expected a positive length in varchar(n)";
    case ERROR_SYNTAX_EXPECTED_TABLE:       return "expected the keyword table";
    case ERROR_SYNTAX_EXPECTED_TO:          return "expected the keyword to";
    case ERROR_SYNTAX_EXPRESSION_TOO_COMPLEX: return "expression is too complex";

    case ERROR_SEMANTIC_TABLE_NOT_FOUND:    return "no such table";
    case ERROR_SEMANTIC_TABLE_EXISTS:       return "table already exists";
    case ERROR_SEMANTIC_COLUMN_NOT_FOUND:   return "no such column";
    case ERROR_SEMANTIC_TYPE_MISMATCH:      return "type mismatch";
    case ERROR_SEMANTIC_COLUMN_COUNT:       return "wrong number of values";
    case ERROR_SEMANTIC_DUPLICATE_COLUMN:   return "duplicate column name";
    case ERROR_SEMANTIC_NOT_GROUPED:        return "column must appear in GROUP BY or an aggregate";
    case ERROR_SEMANTIC_INDEX_EXISTS:       return "index already exists";
    case ERROR_SEMANTIC_INDEX_NOT_FOUND:    return "no such index";
    case ERROR_SEMANTIC_HAVING_WITHOUT_GROUP: return "HAVING requires GROUP BY or an aggregate";
    case ERROR_SEMANTIC_DATABASE_EXISTS:    return "database already exists";
    case ERROR_SEMANTIC_DATABASE_NOT_FOUND: return "no such database";
    case ERROR_SEMANTIC_CANNOT_DROP_DEFAULT: return "the default database cannot be dropped";
    case ERROR_SEMANTIC_AMBIGUOUS_COLUMN:   return "column name matches more than one table";
    case ERROR_SEMANTIC_DUPLICATE_TABLE:    return "the same table is listed twice in FROM";
    case ERROR_SEMANTIC_JOIN_TOO_WIDE:      return "the joined tables have too many columns";
    case ERROR_SEMANTIC_MISSING_VALUE:      return "a column with no value and no default";
    case ERROR_SEMANTIC_INVALID_DATE:       return "not a date: expected 'YYYY-MM-DD'";
    case ERROR_SEMANTIC_LAST_COLUMN:        return "cannot drop the only column of a table";
    case ERROR_SEMANTIC_ALTER_UNSUPPORTED:  return "unique, primary key and check are not supported on a new column";
    case ERROR_SEMANTIC_CHECK_BLOCKS_DROP:  return "cannot drop a column from a table with a check constraint";

    case ERROR_EXEC_TABLE_FULL:             return "table is full";
    case ERROR_EXEC_TOO_MANY_TABLES:        return "too many tables";
    case ERROR_EXEC_TOO_MANY_GROUPS:        return "too many groups";
    case ERROR_EXEC_TOO_MANY_INDEXES:       return "out of memory building the index";
    case ERROR_EXEC_JOIN_TOO_LARGE:         return "the join produced too many rows";
    case ERROR_EXEC_ROW_TOO_LARGE:          return "row is too large to store";
    case ERROR_EXEC_OUT_OF_MEMORY:          return "out of memory";
    case ERROR_EXEC_TOO_MANY_DATABASES:     return "too many databases";
    case ERROR_EXEC_TRANSACTION_ACTIVE:     return "a transaction is already open";
    case ERROR_EXEC_NO_TRANSACTION:         return "no transaction is open";
    case ERROR_EXEC_CANNOT_ROLLBACK:        return "nothing to roll back to: this session has no database file";
    case ERROR_EXEC_NOT_NULL:               return "NULL in a NOT NULL column";
    case ERROR_EXEC_NOT_UNIQUE:             return "duplicate value in a unique column";
    case ERROR_EXEC_CHECK_FAILED:           return "row fails the table's CHECK";
    case ERROR_EXEC_VALUE_TOO_LONG:         return "text is longer than the column allows";
    case ERROR_EXEC_DIVIDE_BY_ZERO:         return "division by zero";
    case ERROR_EXEC_TABLE_TOO_WIDE:         return "too many columns";

    case ERROR_IO_CANNOT_OPEN:              return "cannot open the database file";
    case ERROR_IO_BAD_FORMAT:               return "not a database file, or it is truncated";
    case ERROR_IO_VERSION:                  return "database file version not supported";
    case ERROR_IO_WRITE:                    return "failed writing the database file";
    case ERROR_IO_CHECKSUM:                 return "a page failed its checksum: the database file is damaged";

    default:                                return "error";
    }
}
