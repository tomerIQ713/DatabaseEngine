#include "sql_common.h"

/*
 * Rows live in slotted pages, and the pages live in the buffer pool (14_pool.c)
 * rather than in memory this module owns.
 *
 * A page has a slot directory growing up from the front and record data growing
 * down from the back. A record is written field by field, so a row of two ints
 * costs about a dozen bytes rather than the two kilobytes a fixed Row occupies.
 *
 *   u32 slotCount
 *   u32 dataStart          offset where record data begins, grows downward
 *                          from PAGE_USABLE, never into the pool's trailer
 *   slot i at 8 + i*8:     u32 offset, u32 length     (offset 0 = tombstone)
 *   records packed from PAGE_SIZE downward
 *
 * A row position is (local page << SLOT_BITS) | slot. The page part indexes the
 * heap's own list, not the file, so rows keep their positions no matter which
 * page ids the pool hands out. Positions are stable for the life of a row:
 * DELETE clears a slot's offset but keeps the slot, and only VACUUM renumbers,
 * which is why it rebuilds every index.
 *
 * Every pointer into a page is valid only while that page is pinned, so each
 * function here pins, works, and unpins before returning. Slot counts are
 * mirrored in the heap so that iterating does not have to pin anything at all.
 */

#define SLOT_BITS       10
#define SLOTS_PER_PAGE  (ONE << SLOT_BITS)
#define SLOT_MASK       (SLOTS_PER_PAGE - ONE)
#define PAGE_HEADER     8
#define SLOT_WIDTH      8

/* ---------- little-endian field access, matching the file format ---------- */

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

static unsigned int slotCount(const Page* page) { return getU32(page->data); }
static unsigned int dataStart(const Page* page) { return getU32(page->data + 4); }

static void setSlotCount(Page* page, unsigned int n) { putU32(page->data, n); }
static void setDataStart(Page* page, unsigned int n) { putU32(page->data + 4, n); }

static unsigned char* slotAt(Page* page, int slot)
{
    return page->data + PAGE_HEADER + (size_t)slot * SLOT_WIDTH;
}

static void initPage(Page* page)
{
    memset(page->data, ZERO, PAGE_SIZE);

    /* Records grow down from here, and the trailer is not theirs to grow into:
       the pool stamps a checksum over those last bytes on the way out. */
    setDataStart(page, PAGE_USABLE);
}

/* ---------- record encoding ---------- */

/*
 * A date is a day number, so it is stored, compared and indexed as an int and
 * only differs from one when it is printed or parsed. That is the whole reason
 * to hold it this way: every path that already handles ints handles dates for
 * nothing, and 'YYYY-MM-DD' orders correctly because the number does.
 */
static int intLike(ColType type)
{
    return type == TYPE_INT || type == TYPE_DATE;
}

/*
 * Days from 1970-01-01, by the civil-calendar algorithm: shift the year to
 * start in March so a leap day is the last day of the year and never has to be
 * special-cased, then count eras of 400 years, which is where the calendar
 * repeats exactly.
 */
static long long daysFromCivil(int year, int month, int day)
{
    year -= month <= TWO;

    long long era = (year >= ZERO ? year : year - 399) / 400;
    int       yoe = (int)(year - era * 400);                    /* 0..399 */
    int       doy = (153 * (month + (month > TWO ? -3 : 9)) + TWO) / 5 + day - ONE;
    int       doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;      /* 0..146096 */

    return era * 146097LL + doe - 719468LL;
}

static void civilFromDays(int days, int* year, int* month, int* day)
{
    long long z   = (long long)days + 719468LL;
    long long era = (z >= ZERO ? z : z - 146096LL) / 146097LL;
    int       doe = (int)(z - era * 146097LL);
    int       yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int       doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int       mp  = (5 * doy + TWO) / 153;

    *day   = doy - (153 * mp + TWO) / 5 + ONE;
    *month = mp + (mp < 10 ? 3 : -9);
    *year  = (int)(yoe + era * 400) + (*month <= TWO);
}

static int daysInMonth(int year, int month)
{
    static const int length[12] = { 31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31 };

    if (month == TWO && ((year % 4 == ZERO && year % 100 != ZERO)
                         || year % 400 == ZERO))
        return 29;

    return length[month - ONE];
}

/*
 * Reads exactly 'YYYY-MM-DD'. Anything else - a wrong length, a missing dash,
 * the 31st of February - is not a date, and saying so here is what keeps a
 * date column from filling up with text that only looks like one.
 */
int textToDate(const char* text, int length, int* days)
{
    if (length != 10 || text[4] != '-' || text[7] != '-')
        return ERROR_SEMANTIC_INVALID_DATE;

    int field[3] = { ZERO, ZERO, ZERO };
    int at       = ZERO;

    for (int part = ZERO; part < 3; part++) {
        int digits = part == ZERO ? 4 : TWO;

        for (int d = ZERO; d < digits; d++, at++) {
            if (text[at] < '0' || text[at] > '9')
                return ERROR_SEMANTIC_INVALID_DATE;

            field[part] = field[part] * 10 + (text[at] - '0');
        }
        at++;                                   /* the dash, or the end */
    }

    if (field[1] < ONE || field[1] > 12)
        return ERROR_SEMANTIC_INVALID_DATE;
    if (field[TWO] < ONE || field[TWO] > daysInMonth(field[ZERO], field[1]))
        return ERROR_SEMANTIC_INVALID_DATE;

    *days = (int)daysFromCivil(field[ZERO], field[1], field[TWO]);
    return SUCCESS_CODE;
}

void dateToText(int days, char* out)
{
    int year;
    int month;
    int day;

    civilFromDays(days, &year, &month, &day);
    snprintf(out, 11, "%04d-%02d-%02d", year, month, day);
}

/*
 * Measures the record a row would need. Kept separate from writing it so a page
 * can be chosen before anything is committed to it.
 */
static int recordSize(const Row* row)
{
    int size = TWO;                                 /* column count */

    for (int c = ZERO; c < row->ncols; c++) {
        size++;                                     /* null flag */
        if (row->values[c].isNull)
            continue;

        size++;                                     /* type */
        size += intLike(row->values[c].type)      ? 4
              : row->values[c].type == TYPE_FLOAT ? 8
              : TWO + row->values[c].textLength;
    }

    return size;
}

static void writeRecord(unsigned char* at, const Row* row)
{
    int n = ZERO;

    at[n++] = (unsigned char)(row->ncols & 0xFF);
    at[n++] = (unsigned char)((row->ncols >> 8) & 0xFF);

    for (int c = ZERO; c < row->ncols; c++) {
        const Value* value = &row->values[c];

        at[n++] = (unsigned char)(value->isNull ? ONE : ZERO);
        if (value->isNull)
            continue;

        at[n++] = (unsigned char)value->type;

        if (intLike(value->type)) {
            putU32(at + n, (unsigned int)value->intValue);
            n += 4;
        }
        else if (value->type == TYPE_FLOAT) {
            /* the bytes of the double, in the host's order - the same
               assumption the packed keyword dispatch already makes */
            memcpy(at + n, &value->floatValue, 8);
            n += 8;
        }
        else {
            int length = value->textLength;

            at[n++] = (unsigned char)(length & 0xFF);
            at[n++] = (unsigned char)((length >> 8) & 0xFF);
            memcpy(at + n, valueText(value), (size_t)length);
            n += length;
        }
    }
}

static void readRecord(const unsigned char* at, Row* out)
{
    int n = ZERO;

    *out = (Row){ ZERO };
    out->ncols = (int)at[n] | ((int)at[n + ONE] << 8);
    n += TWO;

    for (int c = ZERO; c < out->ncols && c < MAX_COLS; c++) {
        Value* value = &out->values[c];

        if (at[n++]) {
            value->isNull = ONE;
            value->type   = TYPE_TEXT;              /* refined by the caller's schema */
            continue;
        }

        value->isNull = ZERO;
        value->type   = (ColType)at[n++];

        if (intLike(value->type)) {
            value->intValue = (int)getU32(at + n);
            n += 4;
        }
        else if (value->type == TYPE_FLOAT) {
            memcpy(&value->floatValue, at + n, 8);
            n += 8;
        }
        else {
            int length = (int)at[n] | ((int)at[n + ONE] << 8);
            n += TWO;

            /* Copied out of the page rather than pointed at: the page is
               unpinned the moment this returns, and may then be evicted. */
            setText(value, (const char*)(at + n), length);
            n += length;
        }
    }
}

/* ---------- heaps ---------- */

/* ponytail: flat array of heaps per database, linear lookup by name.
   Fine at MAX_TABLES=64. Hash it when that stops holding. */
static Heap* heaps[MAX_DATABASES][MAX_TABLES];
static int   heapCount[MAX_DATABASES];

void initStorage(void)
{
    for (int d = ZERO; d < MAX_DATABASES; d++)
        heapCount[d] = ZERO;
}

static void clearHeap(Heap* heap)
{
    heap->pageIds   = NULL;
    heap->pageSlots = NULL;
    heap->npages    = ZERO;
    heap->capacity  = ZERO;
    heap->nlive     = ZERO;
    heap->nslots    = ZERO;
}

/*
 * Releases a heap's pages back to the pool. Dropping a table has to do this, or
 * its pages stay allocated for the life of the database.
 */
void heapReset(Heap* heap)
{
    for (int p = ZERO; p < heap->npages; p++)
        poolFree(heap->pageIds[p]);

    free(heap->pageIds);
    free(heap->pageSlots);
    clearHeap(heap);
}

/*
 * Makes room for one more page in the heap's lists. They double rather than
 * growing by one, so a million rows costs about twenty reallocations.
 */
static int growPageList(Heap* heap)
{
    if (heap->npages < heap->capacity)
        return SUCCESS_CODE;

    int  grown = heap->capacity ? heap->capacity * TWO : 8;
    int* ids   = (int*)realloc(heap->pageIds, (size_t)grown * sizeof(int));

    if (ids == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;
    heap->pageIds = ids;

    int* slots = (int*)realloc(heap->pageSlots, (size_t)grown * sizeof(int));
    if (slots == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    heap->pageSlots = slots;
    heap->capacity  = grown;
    return SUCCESS_CODE;
}

static int appendPage(Heap* heap, int* pageId)
{
    int errorCode = growPageList(heap);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    int id;
    errorCode = poolAllocate(&id);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    Page* page = poolPin(id);
    if (page == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    initPage(page);
    poolUnpin(id, ONE);

    heap->pageIds[heap->npages]   = id;
    heap->pageSlots[heap->npages] = ZERO;
    heap->npages++;

    *pageId = id;
    return SUCCESS_CODE;
}

/*
 * Rebuilds the page list from what a save recorded, without reading any of the
 * pages themselves. This is what keeps opening a large database cheap.
 */
int heapAdoptPage(Heap* heap, int pageId, int slotsUsed)
{
    int errorCode = growPageList(heap);
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    heap->pageIds[heap->npages]   = pageId;
    heap->pageSlots[heap->npages] = slotsUsed;
    heap->npages++;
    heap->nslots += slotsUsed;
    return SUCCESS_CODE;
}

void heapSetLive(Heap* heap, int live)      { heap->nlive = live; }
int  heapPageCount(const Heap* heap)        { return heap->npages; }
int  heapPageId(const Heap* heap, int i)    { return heap->pageIds[i]; }
int  heapPageSlots(const Heap* heap, int i) { return heap->pageSlots[i]; }

Heap* findHeap(const char* table)
{
    int db = currentDatabaseId();

    for (int i = ZERO; i < heapCount[db]; i++)
        if (_stricmp(heaps[db][i]->table, table) == ZERO)
            return heaps[db][i];
    return NULL;
}

void renameHeap(const char* from, const char* to)
{
    Heap* heap = findHeap(from);

    if (heap != NULL)
        snprintf(heap->table, NAME_LEN, "%s", to);
}

Heap* createHeap(const char* table)
{
    int db = currentDatabaseId();

    if (heapCount[db] == MAX_TABLES)
        return NULL;

    Heap* heap = (Heap*)malloc(sizeof(Heap));
    if (heap == NULL)
        return NULL;

    snprintf(heap->table, NAME_LEN, "%s", table);
    clearHeap(heap);

    heaps[db][heapCount[db]++] = heap;
    return heap;
}

/*
 * Appends a row and reports where it landed. Only the last page is considered
 * for free space: rows are appended, never backfilled, so earlier pages are
 * full by construction and scanning them would be wasted work.
 */
int heapInsert(Heap* heap, const Row* row, int* position)
{
    int size = recordSize(row);

    if (size > PAGE_SIZE - PAGE_HEADER - SLOT_WIDTH)
        return ERROR_EXEC_ROW_TOO_LARGE;

    int   local  = heap->npages - ONE;
    int   pageId = ZERO;
    Page* page   = NULL;

    if (local >= ZERO) {
        pageId = heap->pageIds[local];
        page   = poolPin(pageId);

        if (page != NULL) {
            unsigned int slots = slotCount(page);
            unsigned int start = dataStart(page);
            unsigned int used  = PAGE_HEADER + (slots + ONE) * SLOT_WIDTH;

            if (slots == SLOTS_PER_PAGE || start < used + (unsigned int)size) {
                poolUnpin(pageId, ZERO);            /* full: start a new one */
                page = NULL;
            }
        }
    }

    if (page == NULL) {
        int errorCode = appendPage(heap, &pageId);
        if (errorCode != SUCCESS_CODE)
            return errorCode;

        local = heap->npages - ONE;
        page  = poolPin(pageId);
        if (page == NULL)
            return ERROR_EXEC_OUT_OF_MEMORY;
    }

    int          slot  = (int)slotCount(page);
    unsigned int start = dataStart(page) - (unsigned int)size;

    writeRecord(page->data + start, row);
    putU32(slotAt(page, slot),     start);
    putU32(slotAt(page, slot) + 4, (unsigned int)size);

    setSlotCount(page, (unsigned int)slot + ONE);
    setDataStart(page, start);
    poolUnpin(pageId, ONE);

    heap->pageSlots[local] = slot + ONE;
    heap->nlive++;
    heap->nslots++;

    *position = (local << SLOT_BITS) | slot;
    return SUCCESS_CODE;
}

/*
 * Slots ever allocated, tombstones included. Not a position range: a page fills
 * on space long before it reaches SLOTS_PER_PAGE, so positions are sparse and
 * scanning 0..heapSlots would walk mostly empty ground. Use heapFirst/heapNext
 * to iterate, and this only for sizing and reporting.
 */
int heapSlots(const Heap* heap) { return heap->nslots; }
int heapLive(const Heap* heap)  { return heap->nlive; }

/*
 * Iteration reads the mirrored slot counts rather than the pages, so walking a
 * table costs nothing until something actually asks for a row.
 */
static Page* pinFor(const Heap* heap, int position, int* pageId,
                    unsigned int* offset, unsigned int* length);

/*
 * Decodes a record in place: text values point at the page's own bytes rather
 * than at a copy, so nothing is allocated and nothing is interned.
 *
 * Two things follow from that. The values live only as long as the pin, and
 * their text is not NUL-terminated - it is (text, textLength), which is what
 * every comparison in this engine already uses.
 *
 * Only columns up to `upto` are decoded, because the fields are variable-length
 * and have to be walked in order. A WHERE on the first column of a wide table
 * therefore stops after one field instead of unpacking the whole row.
 */
void decodeRecord(const unsigned char* at, Row* out, int upto)
{
    int n     = TWO;
    int ncols = (int)at[ZERO] | ((int)at[ONE] << 8);

    out->ncols   = ncols;
    out->deleted = ZERO;

    if (upto >= ncols)
        upto = ncols - ONE;
    if (upto >= MAX_COLS)
        upto = MAX_COLS - ONE;

    for (int c = ZERO; c <= upto; c++) {
        Value* value = &out->values[c];

        if (at[n++]) {
            value->isNull     = ONE;
            value->type       = TYPE_TEXT;          /* refined by the schema */
            value->text       = NULL;
            value->textLength = ZERO;
            continue;
        }

        value->isNull = ZERO;
        value->type   = (ColType)at[n++];

        if (intLike(value->type)) {
            value->intValue   = (int)getU32(at + n);
            value->text       = NULL;
            value->textLength = ZERO;
            n += 4;
        }
        else if (value->type == TYPE_FLOAT) {
            memcpy(&value->floatValue, at + n, 8);
            value->text       = NULL;
            value->textLength = ZERO;
            n += 8;
        }
        else {
            int length = (int)at[n] | ((int)at[n + ONE] << 8);

            n += TWO;
            value->text       = (const char*)(at + n);
            value->textLength = length;
            n += length;
        }
    }
}

/* ---------- scanning ---------- */

static void releaseScanPage(HeapScan* scan)
{
    if (scan->pinned != NULL) {
        poolUnpin(scan->pageId, ZERO);
        scan->pinned = NULL;
        scan->pageId = -1;
    }
}

void heapScanStart(HeapScan* scan, const Heap* heap)
{
    scan->heap   = heap;
    scan->page   = ZERO;
    scan->slot   = ZERO;
    scan->pageId = -1;
    scan->pinned = NULL;
}

/*
 * The next live row, or zero at the end. Tombstones are stepped over here, so a
 * caller never sees one and never has to ask.
 */
int heapScanNext(HeapScan* scan, int* position, const unsigned char** record)
{
    const Heap* heap = scan->heap;

    for (;;) {
        if (scan->page >= heap->npages) {
            releaseScanPage(scan);
            return ZERO;
        }

        if (scan->slot >= heap->pageSlots[scan->page]) {
            releaseScanPage(scan);
            scan->page++;
            scan->slot = ZERO;
            continue;
        }

        if (scan->pinned == NULL) {
            scan->pageId = heap->pageIds[scan->page];
            scan->pinned = poolPin(scan->pageId);

            if (scan->pinned == NULL) {             /* unreadable: stop here */
                scan->pageId = -1;
                return ZERO;
            }
        }

        int          slot   = scan->slot++;
        unsigned int offset = getU32(scan->pinned->data + PAGE_HEADER
                                     + (size_t)slot * SLOT_WIDTH);

        if (offset == ZERO)                         /* tombstone */
            continue;

        *position = (scan->page << SLOT_BITS) | slot;
        *record   = scan->pinned->data + offset;
        return ONE;
    }
}

/* Lets go of whatever the scan was holding. Safe to call twice. */
void heapScanEnd(HeapScan* scan)
{
    releaseScanPage(scan);
}

/*
 * One record by position, for a pass working from a list of positions rather
 * than walking the heap: the rows an index picked out, or the rows a scan kept.
 *
 * The page from the last call stays pinned, so a run of positions on one page
 * costs one pin rather than one per row - which is what every list produced by
 * this engine looks like, because positions come out in page order. Release it
 * with heapScanEnd when the pass is done.
 */
const unsigned char* heapScanAt(HeapScan* scan, int position)
{
    const Heap* heap = scan->heap;
    int         page = position >> SLOT_BITS;
    int         slot = position & SLOT_MASK;

    if (page < ZERO || page >= heap->npages)
        return NULL;
    if (slot < ZERO || slot >= heap->pageSlots[page])
        return NULL;

    if (scan->pinned == NULL || scan->page != page) {
        releaseScanPage(scan);

        scan->page   = page;
        scan->pageId = heap->pageIds[page];
        scan->pinned = poolPin(scan->pageId);

        if (scan->pinned == NULL) {
            scan->pageId = -1;
            return NULL;
        }
    }

    unsigned int offset = getU32(scan->pinned->data + PAGE_HEADER
                                 + (size_t)slot * SLOT_WIDTH);

    if (offset == ZERO)                             /* tombstone */
        return NULL;

    return scan->pinned->data + offset;
}

int heapFirst(const Heap* heap)
{
    for (int page = ZERO; page < heap->npages; page++)
        if (heap->pageSlots[page] > ZERO)
            return page << SLOT_BITS;

    return -1;
}

int heapNext(const Heap* heap, int position)
{
    int page = position >> SLOT_BITS;
    int slot = (position & SLOT_MASK) + ONE;

    if (page < ZERO || page >= heap->npages)
        return -1;

    if (slot < heap->pageSlots[page])
        return (page << SLOT_BITS) | slot;

    for (page++; page < heap->npages; page++)
        if (heap->pageSlots[page] > ZERO)
            return page << SLOT_BITS;

    return -1;
}

/*
 * Pins the page a position lives on and reports where its record sits. The
 * caller unpins what this pins, which is why it hands back the page id.
 */
static Page* pinFor(const Heap* heap, int position, int* pageId,
                    unsigned int* offset, unsigned int* length)
{
    int page = position >> SLOT_BITS;
    int slot = position & SLOT_MASK;

    if (page < ZERO || page >= heap->npages)
        return NULL;
    if (slot < ZERO || slot >= heap->pageSlots[page])
        return NULL;

    *pageId = heap->pageIds[page];

    Page* pinned = poolPin(*pageId);
    if (pinned == NULL)
        return NULL;

    const unsigned char* entry = pinned->data + PAGE_HEADER + (size_t)slot * SLOT_WIDTH;

    *offset = getU32(entry);
    *length = getU32(entry + 4);

    if (*offset == ZERO) {                          /* tombstone */
        poolUnpin(*pageId, ZERO);
        return NULL;
    }

    return pinned;
}


/*
 * Deserialises one row. A tombstoned or out-of-range position is not an error:
 * it comes back with deleted set, which is what every scan already tests.
 */
int heapRead(const Heap* heap, int position, Row* out)
{
    int          pageId;
    unsigned int offset;
    unsigned int length;
    Page*        page = pinFor(heap, position, &pageId, &offset, &length);

    if (page == NULL) {
        *out = (Row){ ZERO };
        out->deleted = ONE;
        return SUCCESS_CODE;
    }

    readRecord(page->data + offset, out);
    out->deleted = ZERO;
    poolUnpin(pageId, ZERO);
    return SUCCESS_CODE;
}

/*
 * Clears a slot's offset, keeping the slot itself so every position after it
 * stays where the indexes expect. The bytes are reclaimed by VACUUM.
 */
int heapMarkDeleted(Heap* heap, int position)
{
    int          pageId;
    unsigned int offset;
    unsigned int length;
    Page*        page = pinFor(heap, position, &pageId, &offset, &length);

    if (page == NULL)
        return SUCCESS_CODE;                        /* already gone */

    putU32(slotAt(page, position & SLOT_MASK), ZERO);
    poolUnpin(pageId, ONE);

    heap->nlive--;
    return SUCCESS_CODE;
}

/*
 * Rewrites the heap onto fresh pages with the tombstones dropped. Positions
 * change, which is why the caller rebuilds the indexes straight afterwards.
 */
int heapCompact(Heap* heap, int* reclaimed)
{
    Heap rebuilt;
    int  slots = heap->nslots;
    Row  row;

    clearHeap(&rebuilt);
    snprintf(rebuilt.table, NAME_LEN, "%s", heap->table);

    for (int position = heapFirst(heap); position >= ZERO;
         position = heapNext(heap, position)) {
        /* heapInsert copies the row into its page, so nothing here outlives
           the iteration - and a large table would otherwise pile up every
           row's text in the statement arena before the first one was used. */
        ArenaMark mark = textMark();

        heapRead(heap, position, &row);

        if (row.deleted) {
            textReset(mark);
            continue;
        }

        int landed;
        int errorCode = heapInsert(&rebuilt, &row, &landed);

        textReset(mark);

        if (errorCode != SUCCESS_CODE) {
            heapReset(&rebuilt);
            return errorCode;
        }
    }

    *reclaimed = slots - rebuilt.nlive;

    heapReset(heap);                                /* releases the old pages */
    heap->pageIds   = rebuilt.pageIds;
    heap->pageSlots = rebuilt.pageSlots;
    heap->npages    = rebuilt.npages;
    heap->capacity  = rebuilt.capacity;
    heap->nlive     = rebuilt.nlive;
    heap->nslots    = rebuilt.nslots;
    return SUCCESS_CODE;
}

int dropHeap(const char* table)
{
    int db = currentDatabaseId();

    for (int i = ZERO; i < heapCount[db]; i++)
        if (_stricmp(heaps[db][i]->table, table) == ZERO) {
            heapReset(heaps[db][i]);
            free(heaps[db][i]);
            heaps[db][i] = heaps[db][--heapCount[db]];
            return SUCCESS_CODE;
        }

    return ERROR_SEMANTIC_TABLE_NOT_FOUND;
}

void freeStorage(void)
{
    for (int d = ZERO; d < MAX_DATABASES; d++) {
        for (int i = ZERO; i < heapCount[d]; i++) {
            heapReset(heaps[d][i]);
            free(heaps[d][i]);
        }
        heapCount[d] = ZERO;
    }
}

/* ---------- value helpers, shared by every stage ---------- */

/*
 * Orders two values of the same type. NULL is not handled here: the callers
 * disagree about what it means, so each applies its own rule first.
 */
/*
 * Text is compared by length and bytes, never as a C string. A value read
 * straight out of a page points at the record's bytes, which carry a length in
 * front of them and no terminator - so strcmp would run into whatever follows.
 *
 * The ordering is the same one strcmp gives for text with no NUL in it, and the
 * same one `compareKeys` in 11_index.c gives: shared prefix first, then length.
 * The scan and the index have to agree, or an index would answer a range query
 * with a different set of rows than the scan it replaces.
 */
static int compareText(const char* a, int an, const char* b, int bn)
{
    int shared = an < bn ? an : bn;
    int order  = shared > ZERO ? memcmp(a, b, (size_t)shared) : ZERO;

    if (order != ZERO)
        return order < ZERO ? -1 : ONE;

    return an < bn ? -1 : an > bn ? ONE : ZERO;
}

int compareValues(const Value* a, const Value* b)
{
    if (intLike(a->type))
        return a->intValue < b->intValue ? -1 : a->intValue > b->intValue ? ONE : ZERO;

    if (a->type == TYPE_FLOAT)
        return a->floatValue < b->floatValue ? -1
             : a->floatValue > b->floatValue ? ONE : ZERO;

    /* memcmp, not _stricmp: identifiers are case-insensitive in this engine but
       stored data is not, so 'BANANA' and 'banana' are different values. */
    return compareText(a->text, a->textLength, b->text, b->textLength);
}

/*
 * Equality does not need an ordering, so it can stop at the lengths: two texts
 * of different lengths are different, and that settles most pairs before a byte
 * is looked at. Only equal-length text reaches the memcmp.
 */
int valuesSame(const Value* a, const Value* b)
{
    if (intLike(a->type))
        return a->intValue == b->intValue;

    if (a->type == TYPE_FLOAT)
        return a->floatValue == b->floatValue;

    return a->textLength == b->textLength
           && (a->textLength == ZERO
               || memcmp(a->text, b->text, (size_t)a->textLength) == ZERO);
}

/*
 * Equality for grouping, where two NULLs do belong in the same group - unlike
 * WHERE, where NULL never equals anything.
 */
int valuesEqual(const Value* a, const Value* b)
{
    if (a->isNull || b->isNull)
        return a->isNull && b->isNull;
    if (a->type != b->type)
        return ZERO;

    return valuesSame(a, b);
}

void setNull(Value* value, ColType type)
{
    value->isNull     = ONE;
    value->type       = type;
    value->intValue   = ZERO;
    value->floatValue = 0.0;
    value->text       = NULL;
    value->textLength = ZERO;
}

void setFloat(Value* value, double number)
{
    value->type       = TYPE_FLOAT;
    value->isNull     = ZERO;
    value->intValue   = ZERO;
    value->floatValue = number;
    value->text       = NULL;
    value->textLength = ZERO;
}

/* Reading a value's text never has to worry about the unset case. */
const char* valueText(const Value* value)
{
    return value->text != NULL ? value->text : "";
}

/*
 * Copies text into the statement arena and points the value at it. Every text
 * value in a query goes through here, which is what makes them all share one
 * lifetime and one place to reclaim.
 */
void setText(Value* value, const char* text, int length)
{
    value->type       = TYPE_TEXT;
    value->isNull     = ZERO;
    value->text       = internText(text, length);
    value->textLength = value->text != NULL ? length : ZERO;
}

/* ---------- the text arena ---------- */

#define ARENA_CHUNK 65536

struct ArenaChunk {
    ArenaChunk* next;
    size_t      used;
    size_t      size;
    char        data[ARENA_CHUNK];
};

/*
 * Bump allocation into chunks that are never moved, so a pointer stays good
 * until the arena is unwound past it. Big values get a chunk of their own
 * rather than forcing the standard chunk size up.
 */
const char* arenaCopy(Arena* arena, const char* text, int length)
{
    size_t need = (size_t)length + ONE;             /* room for the terminator */

    if (arena->head == NULL || arena->head->size - arena->head->used < need) {
        size_t      size  = need > ARENA_CHUNK ? need : ARENA_CHUNK;
        ArenaChunk* chunk = (ArenaChunk*)malloc(sizeof(ArenaChunk) - ARENA_CHUNK + size);

        if (chunk == NULL)
            return NULL;

        chunk->next  = arena->head;
        chunk->used  = ZERO;
        chunk->size  = size;
        arena->head  = chunk;
    }

    char* at = arena->head->data + arena->head->used;

    memcpy(at, text, (size_t)length);
    at[length] = '\0';
    arena->head->used += need;
    return at;
}

void arenaRelease(Arena* arena)
{
    ArenaChunk* chunk = arena->head;

    while (chunk != NULL) {
        ArenaChunk* next = chunk->next;
        free(chunk);
        chunk = next;
    }

    arena->head = NULL;
}

/*
 * One arena per statement. A scan that reads and discards rows can wind it back
 * with textMark/textReset rather than letting every row it rejected pile up.
 */
static Arena statementArena;

const char* internText(const char* text, int length)
{
    return arenaCopy(&statementArena, text, length);
}

void resetTextArena(void)
{
    arenaRelease(&statementArena);
}

ArenaMark textMark(void)
{
    ArenaMark mark;

    mark.chunk = statementArena.head;
    mark.used  = statementArena.head != NULL ? statementArena.head->used : ZERO;
    return mark;
}

/*
 * Unwinds to a mark, freeing whole chunks allocated since. Anything pointing
 * past the mark is dead after this, which is exactly what the caller is saying.
 */
void textReset(ArenaMark mark)
{
    while (statementArena.head != mark.chunk) {
        ArenaChunk* next = statementArena.head->next;
        free(statementArena.head);
        statementArena.head = next;
    }

    if (statementArena.head != NULL)
        statementArena.head->used = mark.used;
}
