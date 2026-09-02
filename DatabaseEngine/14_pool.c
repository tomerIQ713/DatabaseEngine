#include "sql_common.h"

/*
 * The buffer pool: a fixed set of frames holding pages, backed by the database
 * file.
 *
 * The file is opened read-only for the life of a session. A page is read in the
 * first time something asks for it and stays until its frame is needed by
 * someone else, at which point a clean page is simply dropped - the copy in the
 * file is still good. A dirty page has nowhere to go, because appending it to
 * the open file would overwrite the metadata that sits after the pages, and
 * because writing in place would give up the all-or-nothing save that
 * 12_persist.c goes to some trouble to provide. So dirty pages stay resident
 * until a save streams every page into a new file and renames it into place.
 *
 * That is shadow paging, and it has a clear shape: reads are bounded by the
 * pool, writes are bounded by memory.
 *
 * With a log attached the file is opened for writing and the rule changes. A
 * dirty page whose contents are in the log is marked logged, and a logged page
 * can be evicted: it is written into the database file on the way out, and a
 * crash mid-write is repaired by replaying the log. So writes are bounded by
 * the pool too, not by memory - which is the whole point of having a log.
 *
 * A dirty page that is *not* yet logged still cannot go anywhere. That is the
 * write-ahead rule: the log first, the database file second, never the other
 * way round.
 *
 * An in-memory database needs no special case: with no file behind it nothing
 * is ever clean, so nothing is evictable and the pool simply grows.
 *
 * Replacement is CLOCK rather than exact LRU, for two reasons. It costs one
 * step of a rotating hand instead of a scan of every frame, and it does not
 * behave as badly on a table scan: strict LRU evicts precisely the page a
 * repeated scan is about to want next, so a table larger than the pool gets no
 * reuse at all. A page also arrives unreferenced, so one read by a scan that
 * never comes back does not earn it a reprieve, while a page that is touched
 * again - an index root, say - keeps its bit set and survives the sweep.
 */

#define INITIAL_FRAMES 64                   /* 512 KB of cache to start with */

typedef struct {
    int  pageId;                            /* -1 when the frame is free */
    int  pins;
    int  dirty;
    int  logged;                            /* its contents are in the log */
    int  referenced;                        /* CLOCK: survives one sweep */
    Page page;
} Frame;

/* Frames are held one pointer at a time rather than in one block. Growing the
   pool then moves only the pointers, and the address of a page stays fixed for
   as long as anything is holding it - which the B-tree relies on, because it
   keeps a node pinned while descending into a child that may allocate. */
static Frame** frames;
static int     frameCount;
static int    hand;                         /* the rotating CLOCK hand */

/* Where each page is resident, or -1. Page ids are dense, so a plain array
   beats a hash and turns finding a frame into one load instead of a scan of
   every frame on every pin. */
static int* pageFrame;
static int  pageFrameSize;

static FILE* pageFile;                      /* NULL for an in-memory database */
static int   writable;                      /* the file can be written in place */

/* Where the catalog chain starts. The pool carries it because it belongs to the
   file rather than to any one page, and both the log and the header need it. */
static int   catalogRootPage = -1;
static int   pageCount;                     /* pages that exist, holes included */

/* Ids handed back by poolFree, reused before the file is extended. Without this
   a dropped table - or the join scratch heap, rebuilt every query - would grow
   the file forever. */
static int* freeIds;
static int  freeCount;
static int  freeCapacity;

/* Enough to tell demand paging and eviction apart from everything staying
   resident, which is otherwise invisible from the outside. */
static long hits;
static long misses;
static long evictions;

/*
 * Whether the pages in this file carry checksums. Older files do not, and a
 * page of theirs uses the bytes a checksum would sit in, so asking is the only
 * safe thing to do. loadDatabase answers it.
 */
static int checksums;

/* How many pages came back from the file not matching what was written. */
static long corrupt;

void poolSetChecksums(int on)
{
    checksums = on;
}

/*
 * FNV-1a over everything but the trailer, which is where the answer goes. The
 * same function the log uses on its frames, for the same reason: it is cheap,
 * it is not a hash anyone has to agree with, and it catches the bit that got
 * lost.
 *
 * Never returns zero, because zero is what an unstamped page carries.
 */
static unsigned int pageChecksum(const Page* page)
{
    unsigned int sum = 2166136261u;

    for (int i = ZERO; i < PAGE_USABLE; i++) {
        sum ^= page->data[i];
        sum *= 16777619u;
    }

    return sum != ZERO ? sum : ONE;
}

/*
 * Stamps a page on its way out. Called where a page is about to be written
 * down - into the file, or into the log - rather than where it is changed,
 * because a row insert dirties a page and doing 8 KB of arithmetic per row
 * would cost more than storing the row.
 */
void poolStampPage(Page* page)
{
    unsigned int sum = pageChecksum(page);

    page->data[PAGE_USABLE]     = (unsigned char)(sum & 0xFFu);
    page->data[PAGE_USABLE + 1] = (unsigned char)((sum >> 8) & 0xFFu);
    page->data[PAGE_USABLE + 2] = (unsigned char)((sum >> 16) & 0xFFu);
    page->data[PAGE_USABLE + 3] = (unsigned char)((sum >> 24) & 0xFFu);
}

/* Zero when the page was never stamped. */
static unsigned int storedChecksum(const Page* page)
{
    return (unsigned int)page->data[PAGE_USABLE]
         | ((unsigned int)page->data[PAGE_USABLE + 1] << 8)
         | ((unsigned int)page->data[PAGE_USABLE + 2] << 16)
         | ((unsigned int)page->data[PAGE_USABLE + 3] << 24);
}

static long pageOffset(int pageId)
{
    /* page 0 of the file is the header, so data page i starts one page in */
    return (long)PAGE_SIZE * (pageId + ONE);
}

/* ---------- lifecycle ---------- */

static int growFrames(void)
{
    int     grown = frameCount ? frameCount * TWO : INITIAL_FRAMES;
    Frame** moved = (Frame**)realloc(frames, (size_t)grown * sizeof(Frame*));

    if (moved == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    frames = moved;

    for (int i = frameCount; i < grown; i++) {
        frames[i] = (Frame*)malloc(sizeof(Frame));

        if (frames[i] == NULL) {        /* keep whatever was allocated */
            if (i == frameCount)
                return ERROR_EXEC_OUT_OF_MEMORY;
            frameCount = i;
            return SUCCESS_CODE;
        }

        frames[i]->pageId     = -1;
        frames[i]->pins       = ZERO;
        frames[i]->dirty      = ZERO;
        frames[i]->logged     = ZERO;
        frames[i]->referenced = ZERO;
    }

    frameCount = grown;
    return SUCCESS_CODE;
}

/* Keeps the page directory as large as the page count. */
static int growDirectory(int pages)
{
    if (pages <= pageFrameSize)
        return SUCCESS_CODE;

    int  grown = pageFrameSize ? pageFrameSize : 256;
    while (grown < pages)
        grown *= TWO;

    int* moved = (int*)realloc(pageFrame, (size_t)grown * sizeof(int));
    if (moved == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    for (int i = pageFrameSize; i < grown; i++)
        moved[i] = -1;

    pageFrame     = moved;
    pageFrameSize = grown;
    return SUCCESS_CODE;
}

void poolInit(void)
{
    frames       = NULL;
    frameCount   = ZERO;
    hand         = ZERO;
    pageFrame    = NULL;
    pageFrameSize = ZERO;
    pageFile     = NULL;
    pageCount    = ZERO;
    freeIds      = NULL;
    freeCount    = ZERO;
    freeCapacity = ZERO;
    writable     = ZERO;
    catalogRootPage = -1;
    hits         = ZERO;
    misses       = ZERO;
    evictions    = ZERO;
    checksums    = ZERO;
    corrupt      = ZERO;
}

/*
 * Drops every frame and forgets the file. The caller owns whatever happens to
 * the pages: this is called when a database is closed or replaced, at which
 * point their contents are either already saved or deliberately discarded.
 */
void poolClear(void)
{
    for (int i = ZERO; i < frameCount; i++)
        free(frames[i]);

    free(frames);
    free(freeIds);
    free(pageFrame);

    if (pageFile != NULL)
        fclose(pageFile);

    poolInit();
}

/*
 * Hands the pool an open file and the number of pages already in it. Those
 * pages are not read now - that is the whole point - they are faulted in as
 * something asks for them.
 */
void poolAdopt(FILE* file, int pages)
{
    if (pageFile != NULL)
        fclose(pageFile);

    pageFile  = file;
    pageCount = pages;
    writable  = ZERO;                   /* a handle for reading, until told otherwise */
    growDirectory(pages);
}

/*
 * Lets go of the backing file without disturbing the pages already in memory.
 * Save needs this: Windows will not rename over a file that is still open, and
 * every page has just been written to the replacement anyway.
 */
void poolDetachFile(void)
{
    if (pageFile != NULL) {
        fclose(pageFile);
        pageFile = NULL;
    }
}

void poolSetCatalogRoot(int page)
{
    catalogRootPage = page;
}

int poolCatalogRoot(void)
{
    return catalogRootPage;
}

/*
 * Updates the two fields of the header page that move as the database grows.
 * The magic and version are already there and are left alone.
 */
static int poolWriteHeader(void)
{
    unsigned char fields[8];
    unsigned int  values[TWO] = { (unsigned int)pageCount,
                                  (unsigned int)catalogRootPage };

    if (!writable || pageFile == NULL)
        return SUCCESS_CODE;

    for (int v = ZERO; v < TWO; v++)
        for (int b = ZERO; b < 4; b++)
            fields[v * 4 + b] = (unsigned char)((values[v] >> (b * 8)) & 0xFFu);

    if (fseek(pageFile, 12, SEEK_SET) != ZERO
        || fwrite(fields, ONE, 8, pageFile) != 8)
        return ERROR_IO_WRITE;

    return SUCCESS_CODE;
}

/*
 * Pages this session read back damaged. A statement compares it before and
 * after, because a refused page otherwise shows up as a short answer rather
 * than as the failure it is.
 */
long poolCorruptCount(void)
{
    return corrupt;
}

int poolHasDirty(void)
{
    for (int i = ZERO; i < frameCount; i++)
        if (frames[i]->pageId >= ZERO && frames[i]->dirty)
            return ONE;
    return ZERO;
}

/*
 * Forgets every dirty page and takes the file's size back to what it was.
 *
 * This is what a ROLLBACK stands on: BEGIN checkpoints first, so from that
 * point on a dirty page can only be one this transaction wrote, and dropping
 * them all leaves the pool holding exactly what the file holds. The pages
 * allocated since are given up with them, or the header would go on claiming
 * pages the file never got.
 */
void poolRollback(int pages)
{
    for (int i = ZERO; i < frameCount; i++) {
        if (frames[i]->pageId < ZERO || !frames[i]->dirty)
            continue;

        pageFrame[frames[i]->pageId] = -1;
        frames[i]->pageId     = -1;
        frames[i]->pins       = ZERO;
        frames[i]->dirty      = ZERO;
        frames[i]->logged     = ZERO;
        frames[i]->referenced = ZERO;
    }

    if (pages >= ZERO && pages < pageCount)
        pageCount = pages;
}

int poolIsWritable(void)
{
    return writable;
}

int poolPageCount(void)
{
    return pageCount;
}

/*
 * After a save every page is in the file, so nothing has to stay resident any
 * more. Clearing the dirty bits is what makes the pool able to evict again.
 */
void poolMarkAllClean(void)
{
    for (int i = ZERO; i < frameCount; i++) {
        frames[i]->dirty  = ZERO;
        frames[i]->logged = ZERO;
    }
}

/*
 * Takes a handle the pool may write through, which is what lets a logged page
 * be evicted instead of pinned in memory until the next save.
 */
void poolSetWritable(FILE* file, int pages)
{
    if (pageFile != NULL)
        fclose(pageFile);

    pageFile  = file;
    pageCount = pages;
    writable  = file != NULL;
    growDirectory(pages);
}

/* Writes one frame into the database file at its page's offset. */
static int flushFrame(int index)
{
    if (!writable || pageFile == NULL)
        return ERROR_IO_WRITE;

    if (fseek(pageFile, pageOffset(frames[index]->pageId), SEEK_SET) != ZERO)
        return ERROR_IO_WRITE;

    poolStampPage(&frames[index]->page);

    if (fwrite(frames[index]->page.data, ONE, PAGE_SIZE, pageFile) != PAGE_SIZE)
        return ERROR_IO_WRITE;

    frames[index]->dirty  = ZERO;
    frames[index]->logged = ZERO;
    return SUCCESS_CODE;
}

/* ---------- finding a frame ---------- */

static int findFrame(int pageId)
{
    if (pageId < ZERO || pageId >= pageFrameSize)
        return -1;

    return pageFrame[pageId];
}

/*
 * A free frame, or the least recently used one holding a clean unpinned page.
 * Growing is the fallback rather than a failure: a pool that cannot evict is
 * still expected to work, just with a larger memory footprint.
 */
static int acquireFrame(void)
{
    /* Two sweeps at most: the first clears reference bits, the second takes the
       first frame that did not earn one. Anything pinned or dirty is not a
       candidate at all, and if that is everything the pool grows instead. */
    for (int step = ZERO; frameCount > ZERO && step < frameCount * TWO; step++) {
        int i = hand;

        hand = (hand + ONE) % frameCount;

        if (frames[i]->pageId < ZERO)
            return i;

        if (frames[i]->pins > ZERO)
            continue;

        /* Unlogged changes have nowhere safe to go yet. */
        if (frames[i]->dirty && !(writable && frames[i]->logged))
            continue;

        if (frames[i]->referenced) {
            frames[i]->referenced = ZERO;    /* second chance */
            continue;
        }

        if (frames[i]->dirty && flushFrame(i) != SUCCESS_CODE)
            continue;                        /* could not write it: leave it */

        pageFrame[frames[i]->pageId] = -1;
        evictions++;
        return i;
    }

    int previous = frameCount;
    if (growFrames() != SUCCESS_CODE)
        return -1;

    hand = ZERO;
    return previous;                        /* first of the newly added frames */
}

/* ---------- pinning ---------- */

/*
 * Returns the page, reading it from the file if it is not already resident, and
 * keeps it in place until the matching poolUnpin. Every caller holding a
 * pointer into a page must be inside a pin, because eviction can otherwise
 * reuse the frame underneath it.
 */
Page* poolPin(int pageId)
{
    if (pageId < ZERO || pageId >= pageCount)
        return NULL;

    int frame = findFrame(pageId);

    if (frame >= ZERO) {
        hits++;
        frames[frame]->referenced = ONE;     /* touched again: worth keeping */
    }
    else {
        misses++;
        frame = acquireFrame();
        if (frame < ZERO)
            return NULL;

        /* The frame is nobody's until the read below succeeds. Leaving the
           evicted page's id on it would let a later eviction clear a directory
           entry that by then points at some other frame - and the same page
           would end up resident twice, with one copy's changes invisible. */
        frames[frame]->pageId = -1;

        /* A page with no file behind it has never been written and cannot be
           read back; that only happens if a dirty page was somehow dropped. */
        if (pageFile == NULL)
            return NULL;

        if (fseek(pageFile, pageOffset(pageId), SEEK_SET) != ZERO)
            return NULL;

        if (fread(frames[frame]->page.data, ONE, PAGE_SIZE, pageFile) != PAGE_SIZE)
            return NULL;

        /*
         * What came back is not what was written. Refusing it is the whole
         * point: handing the page up would answer a query with corrupted rows,
         * and the caller already treats a page it cannot have as missing.
         */
        if (checksums) {
            unsigned int stored = storedChecksum(&frames[frame]->page);

            if (stored != ZERO && stored != pageChecksum(&frames[frame]->page)) {
                fprintf(stderr, "page %d failed its checksum\n", pageId);
                corrupt++;
                return NULL;
            }
        }

        frames[frame]->pageId     = pageId;
        frames[frame]->dirty      = ZERO;
        frames[frame]->logged     = ZERO;
        frames[frame]->pins       = ZERO;
        frames[frame]->referenced = ZERO;    /* a scan gets no head start */
        pageFrame[pageId]        = frame;
    }

    frames[frame]->pins++;
    return &frames[frame]->page;
}

/*
 * What the pool is doing, for .pool. Misses are page faults; evictions are
 * clean pages dropped to make room. Dirty pages are the ones that cannot be
 * evicted until a save puts them in the file.
 */
void poolReport(void)
{
    int resident = ZERO;
    int dirty    = ZERO;
    int pinned   = ZERO;

    for (int i = ZERO; i < frameCount; i++) {
        if (frames[i]->pageId < ZERO)
            continue;

        resident++;
        if (frames[i]->dirty)
            dirty++;
        if (frames[i]->pins > ZERO)
            pinned++;
    }

    printf("--- buffer pool ---\n");
    printf("log %s", walIsOpen() ? "attached" : "none");
    if (walIsOpen())
        printf(", %ld frame(s)", walFrameCount());
    printf("\n");
    printf("pages %d, file %s\n", pageCount, pageFile ? "attached" : "none (memory only)");
    printf("frames %d, resident %d, dirty %d, pinned %d\n",
           frameCount, resident, dirty, pinned);
    printf("hits %ld, misses %ld, evictions %ld\n", hits, misses, evictions);
    printf("checksums %s, %ld page(s) failed\n",
           checksums ? "on" : "off (older file)", corrupt);
}

void poolUnpin(int pageId, int dirty)
{
    int frame = findFrame(pageId);

    if (frame < ZERO)
        return;

    if (frames[frame]->pins > ZERO)
        frames[frame]->pins--;
    if (dirty) {
        frames[frame]->dirty  = ONE;
        frames[frame]->logged = ZERO;       /* changed again since it was logged */
    }
}

/* ---------- allocation ---------- */

/*
 * A fresh zeroed page, reusing a freed id where there is one. The page starts
 * dirty because nothing in the file corresponds to it yet.
 */
int poolAllocate(int* pageId)
{
    int frame = acquireFrame();
    if (frame < ZERO)
        return ERROR_EXEC_OUT_OF_MEMORY;

    int id = freeCount > ZERO ? freeIds[--freeCount] : pageCount++;

    if (growDirectory(id + ONE) != SUCCESS_CODE)
        return ERROR_EXEC_OUT_OF_MEMORY;

    memset(frames[frame]->page.data, ZERO, PAGE_SIZE);
    frames[frame]->pageId     = id;
    frames[frame]->pins       = ZERO;
    frames[frame]->dirty      = ONE;
    frames[frame]->logged     = ZERO;
    frames[frame]->referenced = ONE;         /* just written, so it is in use */
    pageFrame[id]            = frame;

    *pageId = id;
    return SUCCESS_CODE;
}

/*
 * Returns a page to the free list. The file keeps its slot - holes are cheaper
 * than renumbering every position that points past them.
 */
void poolFree(int pageId)
{
    int frame = findFrame(pageId);

    if (frame >= ZERO) {
        frames[frame]->pageId     = -1;
        frames[frame]->pins       = ZERO;
        frames[frame]->dirty      = ZERO;
        frames[frame]->logged     = ZERO;
        frames[frame]->referenced = ZERO;
        pageFrame[pageId]        = -1;
    }

    if (freeCount == freeCapacity) {
        int  grown = freeCapacity ? freeCapacity * TWO : 16;
        int* moved = (int*)realloc(freeIds, (size_t)grown * sizeof(int));

        if (moved == NULL)
            return;                         /* the id leaks; not worth failing over */

        freeIds      = moved;
        freeCapacity = grown;
    }

    freeIds[freeCount++] = pageId;
}

/*
 * Streams every page to a file, faulting each one in and letting it go again,
 * so saving a database far larger than the pool costs one frame at a time.
 * Freed pages are written as zeroes to keep page ids lined up with offsets.
 */
int poolWriteAll(FILE* file)
{
    static unsigned char blank[PAGE_SIZE];

    for (int id = ZERO; id < pageCount; id++) {
        Page* page = poolPin(id);

        if (page == NULL) {
            if (fwrite(blank, ONE, PAGE_SIZE, file) != PAGE_SIZE)
                return ERROR_IO_WRITE;
            continue;
        }

        poolStampPage(page);

        size_t written = fwrite(page->data, ONE, PAGE_SIZE, file);
        poolUnpin(id, ZERO);

        if (written != PAGE_SIZE)
            return ERROR_IO_WRITE;
    }

    return SUCCESS_CODE;
}

/*
 * Writes every unlogged change into the log and puts the log on disk. When this
 * returns the statement is durable: a crash now is repaired by replaying, and a
 * crash a moment earlier loses the statement whole. Nothing partial survives.
 */
int poolCommit(void)
{
    if (!walIsOpen())
        return SUCCESS_CODE;

    int last = -1;

    for (int i = ZERO; i < frameCount; i++)
        if (frames[i]->pageId >= ZERO && frames[i]->dirty && !frames[i]->logged)
            last = i;

    if (last < ZERO)
        return SUCCESS_CODE;                /* nothing changed */

    for (int i = ZERO; i < frameCount; i++) {
        if (frames[i]->pageId < ZERO || !frames[i]->dirty || frames[i]->logged)
            continue;

        /* The log carries whole pages, and a page replayed into the file has
           to satisfy the same check as one written there directly. */
        poolStampPage(&frames[i]->page);

        int errorCode = walAppend(frames[i]->pageId, &frames[i]->page,
                                  i == last, pageCount, poolCatalogRoot());
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    int errorCode = walSync();
    if (errorCode != SUCCESS_CODE)
        return errorCode;

    /* Only now, with the commit marker on the disk, may these pages move. A
       page marked logged is one the file may be written from, so marking any
       of them before the log is whole would break the write-ahead rule
       precisely when it matters: a half-written transaction. */
    for (int i = ZERO; i < frameCount; i++)
        if (frames[i]->pageId >= ZERO && frames[i]->dirty)
            frames[i]->logged = ONE;

    return SUCCESS_CODE;
}

/*
 * Moves everything the log is holding into the database file and empties it.
 * The order matters: pages out, database file synced, only then the log
 * removed - dropping the log first would leave nothing to recover from.
 */
int poolCheckpoint(void)
{
    if (!writable || pageFile == NULL)
        return SUCCESS_CODE;

    for (int i = ZERO; i < frameCount; i++) {
        if (frames[i]->pageId < ZERO || !frames[i]->dirty)
            continue;

        int errorCode = flushFrame(i);
        if (errorCode != SUCCESS_CODE)
            return errorCode;
    }

    if (poolWriteHeader() != SUCCESS_CODE)
        return ERROR_IO_WRITE;

    /* Flushing only reaches the operating system. The log may not be dropped
       until these pages are on the disk itself. */
    if (walSyncHandle(pageFile) != SUCCESS_CODE)
        return ERROR_IO_WRITE;

    return walTruncate();
}
