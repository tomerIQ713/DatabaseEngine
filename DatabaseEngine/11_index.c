#include "sql_common.h"

/*
 * B+ trees whose nodes are pool pages rather than malloc'd structs.
 *
 * A node is one page. Entries are fixed size so a node is an array rather than
 * a slotted page, which keeps splitting simple; the cost is that a long text
 * key is stored as a prefix of INDEX_KEY_MAX bytes.
 *
 * That truncation is safe, but only because of an invariant the executor
 * already maintains: an index narrows candidates and every candidate is then
 * re-tested against the full condition. Prefix order is monotone with full
 * order - if a < b then prefix(a) <= prefix(b) - so a truncated key can put
 * extra rows in the candidate set but can never hide one, as long as the scans
 * stop only on a strict inequality. They do; see indexScan.
 *
 * An index is on one column, so every key in one tree has the same type, and
 * the entry size follows it. An int key is the whole entry's worth of payload
 * plus four bytes; only text pays for the prefix. That is the difference
 * between an int index costing eight bytes a key and fifty-two.
 *
 *   page header:  u32 flags (bit 0 = isLeaf, bit 1 = text keys), u32 nkeys,
 *                 u32 nextLeaf, u32 lastChild
 *   int entry:    u32 payload, u32 key (sign-biased)
 *   text entry:   u32 payload, u32 fullLength,
 *                 u8 type, u8 storedLength, u8 pad[2], u8 key[INDEX_KEY_MAX]
 *
 * Because nodes are pages they are written and read by the ordinary save and
 * load path, so a tree survives a restart instead of being rebuilt by scanning
 * the table. Every pointer into a node is only valid while its page is pinned.
 */

#define INDEX_KEY_MAX     40
#define INDEX_ENTRY_INT    8
#define INDEX_ENTRY_FLOAT 12
#define INDEX_ENTRY_TEXT  (12 + INDEX_KEY_MAX)
#define INDEX_HEADER      16
/* Sized by the whole page, not by what fits today: freeSubtree walks nodes
   from older files too, and those were filled before the trailer existed. */
#define INDEX_ORDER_MAX   ((PAGE_SIZE - INDEX_HEADER) / INDEX_ENTRY_INT)
#define INVALID_PAGE      0xFFFFFFFFu

#define NODE_LEAF  1u
#define NODE_TEXT  2u
#define NODE_FLOAT 4u
#define NODE_KEY   (NODE_TEXT | NODE_FLOAT)     /* the bits naming the key type */

static Index* indexList[MAX_DATABASES];

void initIndexes(void)
{
    for (int d = ZERO; d < MAX_DATABASES; d++)
        indexList[d] = NULL;
}

/* ---------- raw page access ---------- */

static unsigned int getU32(const unsigned char* at)
{
    return (unsigned int)at[ZERO]
         | ((unsigned int)at[1] << 8)
         | ((unsigned int)at[2] << 16)
         | ((unsigned int)at[3] << 24);
}

static void putU32(unsigned char* at, unsigned int value)
{
    at[ZERO] = (unsigned char)(value & 0xFFu);
    at[1]    = (unsigned char)((value >> 8) & 0xFFu);
    at[2]    = (unsigned char)((value >> 16) & 0xFFu);
    at[3]    = (unsigned char)((value >> 24) & 0xFFu);
}

static int  nodeIsLeaf(const Page* p)   { return (int)(getU32(p->data) & NODE_LEAF); }
static int  nodeIsText(const Page* p)   { return (int)(getU32(p->data) & NODE_TEXT); }
static int  nodeIsFloat(const Page* p)  { return (int)(getU32(p->data) & NODE_FLOAT); }

/* Which key type this node holds, in the form newNode takes, so a split can
   make a sibling of the same shape without knowing what shape that is. */
static unsigned int nodeKeyBits(const Page* p)
{
    return getU32(p->data) & NODE_KEY;
}

static unsigned int keyBitsFor(ColType type)
{
    /* a date is a day count, so it is an int key and orders like one */
    return type == TYPE_TEXT  ? NODE_TEXT
         : type == TYPE_FLOAT ? NODE_FLOAT : 0u;
}

/* One index, one column, one key type - so the layout is a property of the
   node and every entry in it is the same size. */
static int  nodeEntry(const Page* p)
{
    return nodeIsText(p)  ? INDEX_ENTRY_TEXT
         : nodeIsFloat(p) ? INDEX_ENTRY_FLOAT : INDEX_ENTRY_INT;
}

/* The trailer belongs to the pool, so the entries stop before it. */
static int  nodeOrder(const Page* p)
{
    return (PAGE_USABLE - INDEX_HEADER) / nodeEntry(p);
}
static int  nodeKeys(const Page* p)     { return (int)getU32(p->data + 4); }
static int  nodeNextLeaf(const Page* p) { return (int)getU32(p->data + 8); }
static int  nodeLastChild(const Page* p){ return (int)getU32(p->data + 12); }

static void setNodeKeys(Page* p, int n)      { putU32(p->data + 4,  (unsigned int)n); }
static void setNodeNextLeaf(Page* p, int n)  { putU32(p->data + 8,  (unsigned int)n); }
static void setNodeLastChild(Page* p, int n) { putU32(p->data + 12, (unsigned int)n); }

static unsigned char* entryAt(Page* p, int i)
{
    return p->data + INDEX_HEADER + (size_t)i * nodeEntry(p);
}

static int entryPayload(const Page* p, int i)
{
    return (int)getU32(p->data + INDEX_HEADER + (size_t)i * nodeEntry(p));
}

/* ---------- keys as they are stored ---------- */

static void putU64(unsigned char* at, unsigned long long value)
{
    for (int i = ZERO; i < 8; i++)
        at[i] = (unsigned char)((value >> (i * 8)) & 0xFFu);
}

static unsigned long long getU64(const unsigned char* at)
{
    unsigned long long value = ZERO;

    for (int i = 7; i >= ZERO; i--)
        value = (value << 8) | at[i];

    return value;
}

/*
 * A double, as an unsigned integer that sorts the same way the double does.
 *
 * IEEE doubles are already sign-and-magnitude in their bit pattern, so the
 * positives are in order among themselves and so are the negatives - just
 * backwards, and below zero in the wrong half. Flipping every bit of a
 * negative reverses that half and puts it first; setting the top bit of a
 * positive moves it above them. One unsigned comparison then answers what a
 * float comparison would, which is what lets a float key sit in a tree that
 * only knows how to order bytes.
 */
static unsigned long long floatOrder(double number)
{
    unsigned long long bits;

    memcpy(&bits, &number, 8);

    return (bits & 0x8000000000000000ull) ? ~bits : (bits | 0x8000000000000000ull);
}

/* A key lifted out of a page, so it can outlive the pin that produced it. */
typedef struct {
    unsigned char type;
    unsigned char storedLength;
    unsigned int  fullLength;
    unsigned char key[INDEX_KEY_MAX];
} StoredKey;

/*
 * Encodes a value as it is stored. An int is four bytes and never truncates; a
 * text key keeps its first INDEX_KEY_MAX bytes and remembers how long it really
 * was, which is what lets comparison tell "equal" from "equal so far".
 */
static void makeKey(const Value* value, StoredKey* out)
{
    memset(out, ZERO, sizeof *out);
    out->type = (unsigned char)value->type;

    if (value->type == TYPE_FLOAT) {
        putU64(out->key, floatOrder(value->floatValue));
        out->storedLength = 8;
        out->fullLength   = 8;
        return;
    }

    if (value->type == TYPE_DATE) {
        /* stored as the int it is, so one tree serves both */
        unsigned int bits = (unsigned int)value->intValue ^ 0x80000000u;

        putU32(out->key, bits);
        out->type         = (unsigned char)TYPE_INT;
        out->storedLength = 4;
        out->fullLength   = 4;
        return;
    }

    if (value->type == TYPE_INT) {
        /* biased so that the unsigned byte order matches signed order */
        unsigned int bits = (unsigned int)value->intValue ^ 0x80000000u;

        putU32(out->key, bits);
        out->storedLength = 4;
        out->fullLength   = 4;
        return;
    }

    int length = value->textLength;

    out->fullLength   = (unsigned int)length;
    out->storedLength = (unsigned char)(length < INDEX_KEY_MAX ? length : INDEX_KEY_MAX);
    memcpy(out->key, valueText(value), out->storedLength);
}

static void readKey(const Page* p, int i, StoredKey* out)
{
    const unsigned char* at = p->data + INDEX_HEADER + (size_t)i * nodeEntry(p);

    /* Only the bytes the key actually uses are touched. This runs once per
       comparison, so clearing the whole prefix buffer for a four-byte int key
       was most of the cost of a descent. */
    if (nodeIsFloat(p)) {
        out->type         = (unsigned char)TYPE_FLOAT;
        out->storedLength = 8;
        out->fullLength   = 8;
        memcpy(out->key, at + 4, 8);
        return;
    }

    if (!nodeIsText(p)) {
        out->type         = (unsigned char)TYPE_INT;
        out->storedLength = 4;
        out->fullLength   = 4;
        memcpy(out->key, at + 4, 4);
        return;
    }

    memset(out->key, ZERO, INDEX_KEY_MAX);

    out->fullLength   = getU32(at + 4);
    out->type         = at[8];
    out->storedLength = at[9];
    memcpy(out->key, at + 12, INDEX_KEY_MAX);
}

static void writeEntry(Page* p, int i, const StoredKey* key, int payload)
{
    unsigned char* at = entryAt(p, i);

    putU32(at, (unsigned int)payload);

    if (nodeIsFloat(p)) {
        memcpy(at + 4, key->key, 8);
        return;
    }

    if (!nodeIsText(p)) {
        memcpy(at + 4, key->key, 4);
        return;
    }

    putU32(at + 4, key->fullLength);
    at[8]  = key->type;
    at[9]  = key->storedLength;
    at[10] = ZERO;
    at[11] = ZERO;
    memcpy(at + 12, key->key, INDEX_KEY_MAX);
}

static void moveEntry(Page* to, int toIndex, const Page* from, int fromIndex)
{
    int size = nodeEntry(to);

    memcpy(entryAt(to, toIndex),
           from->data + INDEX_HEADER + (size_t)fromIndex * size,
           (size_t)size);
}

/*
 * Orders two stored keys. Ints are exact. Text compares the bytes that are
 * there and then the real lengths, so two keys only compare equal when they
 * genuinely are - unless both were truncated at the same prefix, in which case
 * equal means "indistinguishable from here", and the caller treats it as a
 * candidate rather than an answer.
 */
static int compareKeys(const StoredKey* a, const StoredKey* b)
{
    if (a->type == (unsigned char)TYPE_FLOAT) {
        unsigned long long left  = getU64(a->key);
        unsigned long long right = getU64(b->key);

        return left < right ? -1 : left > right ? ONE : ZERO;
    }

    if (a->type == (unsigned char)TYPE_INT) {
        unsigned int left  = getU32(a->key);
        unsigned int right = getU32(b->key);

        return left < right ? -1 : left > right ? ONE : ZERO;
    }

    int shared = a->storedLength < b->storedLength ? a->storedLength : b->storedLength;
    int order  = memcmp(a->key, b->key, (size_t)shared);

    if (order != ZERO)
        return order < ZERO ? -1 : ONE;

    if (a->storedLength != b->storedLength)
        return a->storedLength < b->storedLength ? -1 : ONE;

    /* both truncated at the same length: nothing here separates them */
    if (a->storedLength == INDEX_KEY_MAX && a->fullLength != b->fullLength)
        return ZERO;

    return a->fullLength < b->fullLength ? -1
         : a->fullLength > b->fullLength ? ONE : ZERO;
}

/* ---------- node allocation ---------- */

static int newNode(int isLeaf, unsigned int keyBits, int* pageId)
{
    int errorCode = poolAllocate(pageId);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    Page* page = poolPin(*pageId);
    if (page == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    memset(page->data, ZERO, PAGE_SIZE);
    putU32(page->data, (isLeaf ? NODE_LEAF : 0u) | keyBits);
    setNodeKeys(page, ZERO);
    setNodeNextLeaf(page, (int)INVALID_PAGE);
    setNodeLastChild(page, (int)INVALID_PAGE);
    poolUnpin(*pageId, ONE);
    return SUCCESS_CODE;
}

/* Releases every page of a subtree. */
static void freeSubtree(int pageId)
{
    if (pageId == (int)INVALID_PAGE || pageId < ZERO)
        return;

    Page* page = poolPin(pageId);
    if (page == NULL)
        return;

    if (!nodeIsLeaf(page)) {
        int keys = nodeKeys(page);
        int children[INDEX_ORDER_MAX + 1];

        for (int i = ZERO; i < keys; i++)
            children[i] = entryPayload(page, i);
        children[keys] = nodeLastChild(page);

        poolUnpin(pageId, ZERO);

        for (int i = ZERO; i <= keys; i++)
            freeSubtree(children[i]);
    }
    else {
        poolUnpin(pageId, ZERO);
    }

    poolFree(pageId);
}

Index* findIndexByName(const char* name)
{
    for (Index* index = indexList[currentDatabaseId()]; index != NULL; index = index->next)
        if (_stricmp(index->name, name) == ZERO)
            return index;
    return NULL;
}

/*
 * Indexes are registered under unqualified column names, but a query may write
 * "users.id". Matching on the bare name keeps the two spellings on the same
 * access path instead of quietly making one of them a full scan.
 */
Index* findIndexOn(const char* table, const char* column)
{
    const char* dot  = strrchr(column, '.');
    const char* bare = dot != NULL ? dot + ONE : column;

    for (Index* index = indexList[currentDatabaseId()]; index != NULL; index = index->next)
        if (_stricmp(index->table, table) == ZERO
            && _stricmp(index->column, bare) == ZERO)
            return index;
    return NULL;
}

/*
 * Equality and ranges can be answered from the tree. NE and the NULL tests
 * cannot: NULLs are never stored, and NE would visit every key anyway.
 */
int indexableOperator(CompareOp op)
{
    return op == OP_EQ || op == OP_LT || op == OP_LTE
        || op == OP_GT || op == OP_GTE;
}

int createIndex(const char* name, const char* table, const char* column,
                int slot, ColType keyType)
{
    Index* index = (Index*)malloc(sizeof(Index));
    if (index == NULL)
        return ERROR_EXEC_TOO_MANY_INDEXES;

    int root;
    int errorCode = newNode(ONE, keyBitsFor(keyType), &root);
    if (errorCode != SUCCESS_CODE) {
        free(index);
        return errorCode;
    }

    snprintf(index->name,   NAME_LEN, "%s", name);
    snprintf(index->table,  NAME_LEN, "%s", table);
    snprintf(index->column, NAME_LEN, "%s", column);
    index->slot     = slot;
    index->rootPage = root;
    index->keyCount = ZERO;

    index->next = indexList[currentDatabaseId()];
    indexList[currentDatabaseId()] = index;
    return SUCCESS_CODE;
}

/*
 * Attaches an index whose tree is already in the file, which is what loading a
 * saved database does instead of rebuilding it from the rows.
 */
int adoptIndex(const char* name, const char* table, const char* column,
               int slot, int rootPage, int keyCount)
{
    Index* index = (Index*)malloc(sizeof(Index));
    if (index == NULL)
        return ERROR_EXEC_TOO_MANY_INDEXES;

    snprintf(index->name,   NAME_LEN, "%s", name);
    snprintf(index->table,  NAME_LEN, "%s", table);
    snprintf(index->column, NAME_LEN, "%s", column);
    index->slot     = slot;
    index->rootPage = rootPage;
    index->keyCount = keyCount;

    index->next = indexList[currentDatabaseId()];
    indexList[currentDatabaseId()] = index;
    return SUCCESS_CODE;
}

int indexRootPage(const Index* index)
{
    return index->rootPage;
}

int indexKeyCount(const Index* index)
{
    return index->keyCount;
}

/*
 * First position in the node whose key is >= the search key. Duplicates are
 * allowed, so this is a lower bound rather than an exact hit.
 */
/*
 * Binary search rather than a walk. With a page holding up to a thousand keys
 * the difference is ten comparisons against five hundred, which is most of what
 * a descent costs.
 */
static int lowerBound(const Page* page, const StoredKey* key)
{
    int low  = ZERO;
    int high = nodeKeys(page);
    StoredKey at;

    while (low < high) {
        int middle = low + (high - low) / TWO;

        readKey(page, middle, &at);

        if (compareKeys(&at, key) < ZERO)
            low = middle + ONE;
        else
            high = middle;
    }

    return low;
}

typedef struct {
    int       didSplit;
    StoredKey key;                      /* separator promoted to the parent */
    int       right;                    /* page id of the new right node */
    int       errorCode;
} Split;

static Split insertInto(int pageId, const StoredKey* key, int rowPosition);

/*
 * Splits a full leaf in half. The separator is a copy of the right node's first
 * key, which is what makes this a B+ tree: every key still appears in a leaf.
 */
static Split splitLeaf(Page* page)
{
    Split split = { ZERO, { ZERO, ZERO, ZERO, { ZERO } }, ZERO, SUCCESS_CODE };

    int rightPage;
    split.errorCode = newNode(ONE, nodeKeyBits(page), &rightPage);
    if (split.errorCode != SUCCESS_CODE)
        return split;

    Page* right = poolPin(rightPage);
    if (right == NULL) {
        split.errorCode = ERROR_EXEC_OUT_OF_MEMORY;
        return split;
    }

    int half  = nodeOrder(page) / TWO;
    int moved = nodeKeys(page) - half;

    for (int i = ZERO; i < moved; i++)
        moveEntry(right, i, page, half + i);

    setNodeKeys(right, moved);
    setNodeKeys(page, half);

    setNodeNextLeaf(right, nodeNextLeaf(page));
    setNodeNextLeaf(page, rightPage);

    readKey(right, ZERO, &split.key);
    poolUnpin(rightPage, ONE);

    split.didSplit = ONE;
    split.right    = rightPage;
    return split;
}

/*
 * Splits a full internal node. Unlike a leaf split the middle key moves up
 * rather than being copied, so it stops appearing at this level.
 */
static Split splitInternal(Page* page)
{
    Split split = { ZERO, { ZERO, ZERO, ZERO, { ZERO } }, ZERO, SUCCESS_CODE };

    int rightPage;
    split.errorCode = newNode(ZERO, nodeKeyBits(page), &rightPage);
    if (split.errorCode != SUCCESS_CODE)
        return split;

    Page* right = poolPin(rightPage);
    if (right == NULL) {
        split.errorCode = ERROR_EXEC_OUT_OF_MEMORY;
        return split;
    }

    int keys   = nodeKeys(page);
    int middle = keys / TWO;

    readKey(page, middle, &split.key);

    int moved = keys - middle - ONE;
    for (int i = ZERO; i < moved; i++)
        moveEntry(right, i, page, middle + ONE + i);

    setNodeKeys(right, moved);
    setNodeLastChild(right, nodeLastChild(page));
    setNodeLastChild(page, entryPayload(page, middle));
    setNodeKeys(page, middle);

    poolUnpin(rightPage, ONE);

    split.didSplit = ONE;
    split.right    = rightPage;
    return split;
}

static Split insertIntoLeaf(Page* page, const StoredKey* key, int rowPosition)
{
    Split split = { ZERO, { ZERO, ZERO, ZERO, { ZERO } }, ZERO, SUCCESS_CODE };

    int at   = lowerBound(page, key);
    int keys = nodeKeys(page);

    for (int i = keys; i > at; i--)
        moveEntry(page, i, page, i - ONE);

    writeEntry(page, at, key, rowPosition);
    setNodeKeys(page, keys + ONE);

    if (keys + ONE == nodeOrder(page))
        return splitLeaf(page);
    return split;
}

static Split insertInto(int pageId, const StoredKey* key, int rowPosition)
{
    Split split = { ZERO, { ZERO, ZERO, ZERO, { ZERO } }, ZERO, SUCCESS_CODE };

    Page* page = poolPin(pageId);
    if (page == NULL) {
        split.errorCode = ERROR_EXEC_OUT_OF_MEMORY;
        return split;
    }

    if (nodeIsLeaf(page)) {
        split = insertIntoLeaf(page, key, rowPosition);
        poolUnpin(pageId, ONE);
        return split;
    }

    int at    = lowerBound(page, key);
    int keys  = nodeKeys(page);
    int child = at < keys ? entryPayload(page, at) : nodeLastChild(page);

    /* the page stays pinned across the descent, so the child cannot evict it */
    Split below = insertInto(child, key, rowPosition);

    if (below.errorCode != SUCCESS_CODE || !below.didSplit) {
        poolUnpin(pageId, below.didSplit ? ONE : ZERO);
        split.errorCode = below.errorCode;
        return split;
    }

    for (int i = keys; i > at; i--)
        moveEntry(page, i, page, i - ONE);

    /* entry `at` now carries the separator and keeps pointing at the child we
       descended into; the new right node takes the slot just past it */
    writeEntry(page, at, &below.key, child);

    if (at + ONE <= keys) {
        unsigned char* next = entryAt(page, at + ONE);
        putU32(next, (unsigned int)below.right);
    }
    else {
        setNodeLastChild(page, below.right);
    }

    setNodeKeys(page, keys + ONE);

    if (keys + ONE == nodeOrder(page))
        split = splitInternal(page);

    poolUnpin(pageId, ONE);
    return split;
}

int indexInsert(Index* index, const Value* key, int rowPosition)
{
    if (key->isNull)                    /* NULLs are not indexed: nothing matches them */
        return SUCCESS_CODE;

    StoredKey stored;
    makeKey(key, &stored);

    Split split = insertInto(index->rootPage, &stored, rowPosition);
    if (split.errorCode != SUCCESS_CODE)
        return split.errorCode;

    index->keyCount++;

    if (split.didSplit) {               /* the root split, so the tree grew a level */
        int rootPage;
        int errorCode = newNode(ZERO, keyBitsFor((ColType)stored.type), &rootPage);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        Page* root = poolPin(rootPage);
        if (root == NULL)
            return ERROR_EXEC_OUT_OF_MEMORY;

        writeEntry(root, ZERO, &split.key, index->rootPage);
        setNodeKeys(root, ONE);
        setNodeLastChild(root, split.right);
        poolUnpin(rootPage, ONE);

        index->rootPage = rootPage;
    }

    return SUCCESS_CODE;
}

/*
 * Adds one freshly stored row to every index on its table.
 */
int indexInsertRow(const char* table, const Row* row, int rowPosition)
{
    for (Index* index = indexList[currentDatabaseId()]; index != NULL; index = index->next) {
        if (_stricmp(index->table, table) != ZERO)
            continue;

        int errorCode = indexInsert(index, &row->values[index->slot], rowPosition);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }
    return SUCCESS_CODE;
}

/* ---------- scanning ---------- */

static int leftmostLeaf(int pageId)
{
    for (;;) {
        Page* page = poolPin(pageId);
        if (page == NULL)
            return (int)INVALID_PAGE;

        if (nodeIsLeaf(page)) {
            poolUnpin(pageId, ZERO);
            return pageId;
        }

        int child = nodeKeys(page) > ZERO ? entryPayload(page, ZERO)
                                          : nodeLastChild(page);
        poolUnpin(pageId, ZERO);
        pageId = child;
    }
}

/*
 * Descends to the leaf that would hold the first key >= the search key.
 */
static int seekLeaf(int pageId, const StoredKey* key)
{
    for (;;) {
        Page* page = poolPin(pageId);
        if (page == NULL)
            return (int)INVALID_PAGE;

        if (nodeIsLeaf(page)) {
            poolUnpin(pageId, ZERO);
            return pageId;
        }

        int at    = lowerBound(page, key);
        int child = at < nodeKeys(page) ? entryPayload(page, at)
                                        : nodeLastChild(page);
        poolUnpin(pageId, ZERO);
        pageId = child;
    }
}

/*
 * Collects rows whose text key starts with the given prefix. This is what lets
 * LIKE 'abc%' use the tree: every match must sort inside [abc, abd), so a seek
 * plus a forward walk beats reading the whole table. Callers still have to
 * apply the full pattern - the prefix only narrows the candidates.
 */
int indexPrefixScan(const Index* index, const char* prefix,
                    int* rows, int maxRows, int* nrows)
{
    *nrows = ZERO;

    Value seek;
    seek.type       = TYPE_TEXT;
    seek.isNull     = ZERO;
    seek.intValue   = ZERO;
    seek.text       = prefix;
    seek.textLength = (int)strlen(prefix);

    StoredKey target;
    makeKey(&seek, &target);

    /* only the bytes the tree actually stores can be compared here */
    int compareLength = target.storedLength;

    for (int leafPage = seekLeaf(index->rootPage, &target);
         leafPage != (int)INVALID_PAGE;) {

        Page* leaf = poolPin(leafPage);
        if (leaf == NULL)
            return ERROR_EXEC_OUT_OF_MEMORY;

        int keys = nodeKeys(leaf);
        int next = nodeNextLeaf(leaf);

        for (int i = ZERO; i < keys; i++) {
            StoredKey at;
            readKey(leaf, i, &at);

            int shared = at.storedLength < compareLength ? at.storedLength
                                                         : compareLength;
            int order  = memcmp(at.key, target.key, (size_t)shared);

            if (order == ZERO && at.storedLength < compareLength)
                order = -1;             /* shorter key sorts before the prefix */

            if (order > ZERO) {         /* keys ascend, so the range is over */
                poolUnpin(leafPage, ZERO);
                return SUCCESS_CODE;
            }
            if (order < ZERO)           /* seek can land just left of the range */
                continue;

            if (*nrows == maxRows) {
                poolUnpin(leafPage, ZERO);
                return ERROR_EXEC_TABLE_FULL;
            }
            rows[(*nrows)++] = entryPayload(leaf, i);
        }

        poolUnpin(leafPage, ZERO);
        leafPage = next;
    }

    return SUCCESS_CODE;
}

/*
 * Walks the leaf chain collecting row positions that satisfy the predicate.
 * Ranges open on the left (< and <=) have to start at the first leaf; the
 * others can start at the seek position, which is the whole point of the tree.
 *
 * Every test here is deliberately generous. A truncated key compares equal to
 * anything sharing its prefix, so "equal" means "cannot be ruled out" - the
 * scan therefore stops only on a strict inequality and admits ties. The
 * executor re-tests every row it gets, so extra candidates cost time and never
 * correctness, while a missed one would be a wrong answer.
 */
int indexScan(const Index* index, CompareOp op, const Value* key,
              int* rows, int maxRows, int* nrows)
{
    *nrows = ZERO;

    StoredKey target;
    makeKey(key, &target);

    int openLeft = (op == OP_LT || op == OP_LTE);
    int leafPage = openLeft ? leftmostLeaf(index->rootPage)
                            : seekLeaf(index->rootPage, &target);

    while (leafPage != (int)INVALID_PAGE) {
        Page* leaf = poolPin(leafPage);
        if (leaf == NULL)
            return ERROR_EXEC_OUT_OF_MEMORY;

        int keys = nodeKeys(leaf);
        int next = nodeNextLeaf(leaf);

        for (int i = ZERO; i < keys; i++) {
            StoredKey at;
            readKey(leaf, i, &at);

            int comparison = compareKeys(&at, &target);

            if (comparison > ZERO
                && (op == OP_EQ || op == OP_LT || op == OP_LTE)) {
                poolUnpin(leafPage, ZERO);
                return SUCCESS_CODE;
            }

            int matches;
            switch (op) {
            case OP_EQ:  matches = comparison == ZERO; break;
            case OP_LT:  matches = comparison <= ZERO; break;
            case OP_LTE: matches = comparison <= ZERO; break;
            case OP_GT:  matches = comparison >= ZERO; break;
            case OP_GTE: matches = comparison >= ZERO; break;
            default:     matches = ZERO;               break;
            }

            if (!matches)
                continue;

            if (*nrows == maxRows) {
                poolUnpin(leafPage, ZERO);
                return ERROR_EXEC_TABLE_FULL;
            }
            rows[(*nrows)++] = entryPayload(leaf, i);
        }

        poolUnpin(leafPage, ZERO);
        leafPage = next;
    }

    return SUCCESS_CODE;
}

/* ---------- lifecycle ---------- */

static void freeIndex(Index* index)
{
    freeSubtree(index->rootPage);
    free(index);
}

void freeIndexes(void)
{
    for (int d = ZERO; d < MAX_DATABASES; d++) {
        Index* index = indexList[d];

        while (index != NULL) {
            Index* next = index->next;
            free(index);                /* pages go with the pool, not one by one */
            index = next;
        }
        indexList[d] = NULL;
    }
}

/* What kind of key this index's nodes carry, read from the root. */
static unsigned int indexKeyBits(const Index* index)
{
    Page* root = poolPin(index->rootPage);
    if (root == NULL)
        return ZERO;

    unsigned int bits = nodeKeyBits(root);
    poolUnpin(index->rootPage, ZERO);
    return bits;
}

static int treeDepth(int pageId)
{
    int depth = ONE;

    for (;;) {
        Page* page = poolPin(pageId);
        if (page == NULL)
            return depth;

        if (nodeIsLeaf(page)) {
            poolUnpin(pageId, ZERO);
            return depth;
        }

        int child = nodeKeys(page) > ZERO ? entryPayload(page, ZERO)
                                          : nodeLastChild(page);
        poolUnpin(pageId, ZERO);
        pageId = child;
        depth++;
    }
}

void printIndexes(void)
{
    if (indexList[currentDatabaseId()] == NULL) {
        printf("--- no indexes ---\n");
        return;
    }

    printf("--- indexes (%s) ---\n", currentDatabaseName());
    for (Index* index = indexList[currentDatabaseId()]; index != NULL; index = index->next)
        printf("%s on %s(%s): %d key(s), depth %d\n",
               index->name, index->table, index->column,
               index->keyCount, treeDepth(index->rootPage));
}

/*
 * Throws away each index on the table and rebuilds it from the live rows.
 * Cheaper to write than B-tree deletion, and it is what VACUUM needs anyway:
 * after compaction every row position has moved.
 */
int rebuildIndexes(const char* table, const Heap* heap)
{
    for (Index* index = indexList[currentDatabaseId()]; index != NULL; index = index->next) {
        if (_stricmp(index->table, table) != ZERO)
            continue;

        unsigned int keyBits = indexKeyBits(index);

        freeSubtree(index->rootPage);
        index->keyCount = ZERO;

        int errorCode = newNode(ONE, keyBits, &index->rootPage);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        for (int r = heapFirst(heap); r >= ZERO; r = heapNext(heap, r)) {
            ArenaMark mark = textMark();
            Row       row;

            heapRead(heap, r, &row);
            errorCode = row.deleted ? SUCCESS_CODE
                                    : indexInsert(index, &row.values[index->slot], r);
            textReset(mark);

            if (errorCode != SUCCESS_CODE)
                return errorCode;
        }
    }

    return SUCCESS_CODE;
}

int dropIndexByName(const char* name)
{
    Index** link = &indexList[currentDatabaseId()];

    while (*link != NULL) {
        if (_stricmp((*link)->name, name) == ZERO) {
            Index* dead = *link;
            *link = dead->next;
            freeIndex(dead);
            return SUCCESS_CODE;
        }
        link = &(*link)->next;
    }

    return ERROR_SEMANTIC_INDEX_NOT_FOUND;
}

/*
 * Drops every index on a table, for DROP TABLE.
 */
void dropIndexesForTable(const char* table)
{
    Index** link = &indexList[currentDatabaseId()];

    while (*link != NULL) {
        if (_stricmp((*link)->table, table) == ZERO) {
            Index* dead = *link;
            *link = dead->next;
            freeIndex(dead);
            continue;                       /* *link already points at the next */
        }
        link = &(*link)->next;
    }
}

int listIndexes(const Index** out, int max)
{
    int count = ZERO;

    for (const Index* index = indexList[currentDatabaseId()]; index != NULL && count < max;
         index = index->next)
        out[count++] = index;

    return count;
}
