#include "sql_common.h"

/*
 * Databases are namespaces, not separate stores: the catalog, heap list and
 * index list in the other modules each carry a per-database dimension, and
 * every lookup goes through currentDatabaseId(). Switching databases is
 * therefore just moving an integer.
 *
 * Slots are never compacted. An id is the index into those per-database arrays,
 * so renumbering after a DROP would silently repoint every table and index at
 * another database's data. Dropping just marks the slot free for reuse.
 *
 * ponytail: MAX_DATABASES is a fixed 8 and names are searched linearly.
 * Neither is worth improving until someone actually keeps dozens of them.
 */
static char names[MAX_DATABASES][NAME_LEN];
static int  used[MAX_DATABASES];
static THREAD_LOCAL int  current;

/*
 * Slot 0 is the default database. It always exists, which is what lets USE
 * always have somewhere to fall back to.
 */
void initDatabases(void)
{
    for (int i = ZERO; i < MAX_DATABASES; i++)
        used[i] = ZERO;

    snprintf(names[ZERO], NAME_LEN, "%s", DEFAULT_DB_NAME);
    used[ZERO] = ONE;

    /* Thread-local, and zero-initialised - so a connection thread starts
       in the default database without being told, which is where a new
       session should begin. */
    current    = ZERO;
}

/*
 * A session remembers which database USE left it on, and puts it back before
 * each of its statements - so two connections can sit in different databases
 * without either seeing the other move.
 */
void setCurrentDatabaseId(int id)
{
    if (id >= ZERO && id < MAX_DATABASES && used[id])
        current = id;
}

int findDatabase(const char* name)
{
    for (int i = ZERO; i < MAX_DATABASES; i++)
        if (used[i] && _stricmp(names[i], name) == ZERO)
            return i;
    return -1;
}

int createDatabase(const char* name)
{
    if (findDatabase(name) >= ZERO)
        return ERROR_SEMANTIC_DATABASE_EXISTS;

    for (int i = ZERO; i < MAX_DATABASES; i++)
        if (!used[i]) {
            snprintf(names[i], NAME_LEN, "%s", name);
            used[i] = ONE;
            return SUCCESS_CODE;
        }

    return ERROR_EXEC_TOO_MANY_DATABASES;
}

int useDatabase(const char* name)
{
    int id = findDatabase(name);
    if (id < ZERO)
        return ERROR_SEMANTIC_DATABASE_NOT_FOUND;

    current = id;
    return SUCCESS_CODE;
}

/*
 * Marks a slot free. The caller is responsible for having emptied it first;
 * dropping the current database leaves the session in the default one.
 */
void releaseDatabase(int id)
{
    used[id] = ZERO;

    if (current == id)
        current = ZERO;
}

int currentDatabaseId(void)
{
    return current;
}

const char* currentDatabaseName(void)
{
    return names[current];
}

int databaseCount(void)
{
    int count = ZERO;

    for (int i = ZERO; i < MAX_DATABASES; i++)
        if (used[i])
            count++;
    return count;
}

int databaseSlotCount(void)
{
    return MAX_DATABASES;
}

int databaseInUse(int id)
{
    return used[id];
}

const char* databaseName(int id)
{
    return names[id];
}

/*
 * Used by save, load and DROP DATABASE, which all walk slots directly.
 */
void selectDatabaseById(int id)
{
    current = id;
}

void printDatabases(void)
{
    printf("--- databases ---\n");

    for (int i = ZERO; i < MAX_DATABASES; i++)
        if (used[i])
            printf("%s%s\n", names[i], i == current ? "  (current)" : "");
}
