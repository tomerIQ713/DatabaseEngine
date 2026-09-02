#include "sql_common.h"

#ifdef _WIN32
#include <io.h>
#define syncFile(f) _commit(_fileno(f))
#else
#include <unistd.h>
#define syncFile(f) fsync(fileno(f))
#endif

/*
 * The write-ahead log.
 *
 * The rule the whole thing rests on is that a change reaches the log before it
 * reaches the database file. Once a statement's pages are in the log and the log
 * is on disk, the statement has happened: a crash after that point is repaired
 * by replaying the log, and a crash before it leaves a database that never saw
 * the statement at all. Nothing in between is possible, which is what makes a
 * commit atomic.
 *
 * Frames carry whole pages rather than descriptions of what changed. That costs
 * more log than a diff would, and buys idempotent replay - applying a frame
 * twice is the same as applying it once - so recovery needs no undo pass and no
 * bookkeeping about what it has already done.
 *
 *   header:  magic "MINIWAL1", u32 version, u32 page size
 *   frame:   u32 pageId, u32 commit, u32 pageCount, u32 catalogRoot,
 *            u32 checksum, u32 reserved, then one page
 *
 * A frame with commit set ends a transaction and carries the database's size
 * and catalog root as of that commit, so recovery can restore the header too.
 * Frames after the last commit marker are a torn transaction and are ignored.
 */

#define WAL_MAGIC     "MINIWAL1"
#define WAL_MAGIC_LEN 8
#define WAL_VERSION   1u
#define WAL_HEADER    16
#define FRAME_HEADER  24

static FILE* walFile;
static char  walPath[LINE_LEN];
static char  walDatabase[LINE_LEN];
static long  walFrames;

/* ---------- bytes ---------- */

static void putU32(unsigned char* at, unsigned int value)
{
    at[ZERO] = (unsigned char)(value & 0xFFu);
    at[1]    = (unsigned char)((value >> 8) & 0xFFu);
    at[2]    = (unsigned char)((value >> 16) & 0xFFu);
    at[3]    = (unsigned char)((value >> 24) & 0xFFu);
}

static unsigned int getU32(const unsigned char* at)
{
    return (unsigned int)at[ZERO]
         | ((unsigned int)at[1] << 8)
         | ((unsigned int)at[2] << 16)
         | ((unsigned int)at[3] << 24);
}

/*
 * A frame is only trusted if its checksum matches. Recovery reads a log that a
 * crash may have cut in half, so "is this frame whole" has to be answerable
 * from the frame itself.
 */
static unsigned int checksum(const unsigned char* header, const unsigned char* page)
{
    unsigned int sum = 2166136261u;

    for (int i = ZERO; i < 16; i++) {           /* the fields before the checksum */
        sum ^= header[i];
        sum *= 16777619u;
    }

    for (int i = ZERO; i < PAGE_SIZE; i++) {
        sum ^= page[i];
        sum *= 16777619u;
    }

    return sum;
}

/*
 * fsync, for anyone else who needs it. The pool does: a checkpoint that only
 * flushed would drop the log while the pages were still in the operating
 * system's cache, and a crash there loses exactly what the log was holding.
 */
int walSyncHandle(FILE* file)
{
    if (file == NULL)
        return SUCCESS_CODE;

    if (fflush(file) != ZERO || syncFile(file) != ZERO)
        return ERROR_IO_WRITE;

    return SUCCESS_CODE;
}

static void walName(const char* dbPath, char* out, size_t size)
{
    snprintf(out, size, "%s-wal", dbPath);
}

/* ---------- opening ---------- */

static int writeWalHeader(void)
{
    unsigned char header[WAL_HEADER];

    memset(header, ZERO, sizeof header);
    memcpy(header, WAL_MAGIC, WAL_MAGIC_LEN);
    putU32(header + 8, WAL_VERSION);
    putU32(header + 12, (unsigned int)PAGE_SIZE);

    if (fseek(walFile, ZERO, SEEK_SET) != ZERO
        || fwrite(header, ONE, WAL_HEADER, walFile) != WAL_HEADER)
        return ERROR_IO_WRITE;

    walFrames = ZERO;
    return SUCCESS_CODE;
}

int walOpen(const char* dbPath)
{
    snprintf(walDatabase, sizeof walDatabase, "%s", dbPath);
    walName(dbPath, walPath, sizeof walPath);

    walFile = fopen(walPath, "r+b");

    if (walFile == NULL) {
        walFile = fopen(walPath, "w+b");
        if (walFile == NULL)
            return ERROR_IO_CANNOT_OPEN;
    }

    if (fseek(walFile, ZERO, SEEK_END) != ZERO)
        return ERROR_IO_WRITE;

    long size = ftell(walFile);

    if (size < WAL_HEADER)                      /* fresh log: lay the header down */
        return writeWalHeader();

    walFrames = (size - WAL_HEADER) / (FRAME_HEADER + PAGE_SIZE);
    return SUCCESS_CODE;
}

/*
 * Closes the log and removes it. Only called once its contents are safely in
 * the database file, so an empty log left behind would be noise, and a log left
 * behind would be replayed for no reason on the next open.
 */
void walClose(void)
{
    if (walFile == NULL)
        return;

    fclose(walFile);
    walFile = NULL;
    walFrames = ZERO;
    remove(walPath);
}

int walIsOpen(void)
{
    return walFile != NULL;
}

long walFrameCount(void)
{
    return walFrames;
}

/* ---------- appending ---------- */

/*
 * Adds one page image. Nothing is durable until walCommit, so a statement that
 * fails halfway leaves frames behind that no commit marker ever claims, and
 * recovery steps straight over them.
 */
int walAppend(int pageId, const Page* page, int commit, int pageCount, int catalogRoot)
{
    if (walFile == NULL)
        return SUCCESS_CODE;                    /* no log: nothing to write */

    unsigned char header[FRAME_HEADER];

    putU32(header,      (unsigned int)pageId);
    putU32(header + 4,  (unsigned int)commit);
    putU32(header + 8,  (unsigned int)pageCount);
    putU32(header + 12, (unsigned int)catalogRoot);
    putU32(header + 16, checksum(header, page->data));
    putU32(header + 20, ZERO);

    if (fseek(walFile, ZERO, SEEK_END) != ZERO)
        return ERROR_IO_WRITE;

    if (fwrite(header, ONE, FRAME_HEADER, walFile) != FRAME_HEADER
        || fwrite(page->data, ONE, PAGE_SIZE, walFile) != PAGE_SIZE)
        return ERROR_IO_WRITE;

    walFrames++;
    return SUCCESS_CODE;
}

/*
 * Puts the log on the disk. Until this returns the statement is not committed,
 * however many frames have been written.
 */
int walSync(void)
{
    if (walFile == NULL)
        return SUCCESS_CODE;

    if (fflush(walFile) != ZERO)
        return ERROR_IO_WRITE;
    if (syncFile(walFile) != ZERO)
        return ERROR_IO_WRITE;

    return SUCCESS_CODE;
}

/*
 * Empties the log. Only safe once every page it describes is in the database
 * file and that file is itself on disk - which is what a checkpoint arranges.
 */
int walTruncate(void)
{
    if (walFile == NULL)
        return SUCCESS_CODE;

    fclose(walFile);
    remove(walPath);

    walFile = fopen(walPath, "w+b");
    if (walFile == NULL) {
        walFrames = ZERO;
        return ERROR_IO_CANNOT_OPEN;
    }

    return writeWalHeader();
}

/*
 * Throws away the log belonging to a file that has just been written whole.
 * Every page is in that file now, so a log for it describes a database that no
 * longer exists - and replaying it later would write pages of one generation
 * of the file over another.
 */
void walDiscard(const char* dbPath)
{
    char path[LINE_LEN];

    walName(dbPath, path, sizeof path);

    if (walFile != NULL && _stricmp(walDatabase, dbPath) == ZERO) {
        walTruncate();                          /* ours, and still open */
        return;
    }

    remove(path);
}

/* ---------- recovery ---------- */


/*
 * Replays a log into its database file. Runs before the database is opened, so
 * it works on the file directly rather than through the pool.
 *
 * Only frames up to the last commit marker are applied. Anything after it
 * belongs to a transaction that never finished, and applying it would invent a
 * state the database was never in.
 */
int walRecover(const char* dbPath, int* recovered)
{
    char path[LINE_LEN];
    walName(dbPath, path, sizeof path);

    *recovered = ZERO;

    FILE* log = fopen(path, "rb");
    if (log == NULL)
        return SUCCESS_CODE;                    /* no log, nothing to do */

    unsigned char header[WAL_HEADER];

    if (fread(header, ONE, WAL_HEADER, log) != WAL_HEADER
        || memcmp(header, WAL_MAGIC, WAL_MAGIC_LEN) != ZERO
        || getU32(header + 12) != (unsigned int)PAGE_SIZE) {
        fclose(log);
        remove(path);                           /* not ours, or not usable */
        return SUCCESS_CODE;
    }

    /* First pass: how far does the log commit to? */
    long          frames    = ZERO;
    long          lastGood  = -1;
    unsigned int  finalPages = ZERO;
    unsigned int  finalRoot  = 0xFFFFFFFFu;
    unsigned char frame[FRAME_HEADER];
    static unsigned char image[PAGE_SIZE];

    for (;;) {
        if (fread(frame, ONE, FRAME_HEADER, log) != FRAME_HEADER)
            break;
        if (fread(image, ONE, PAGE_SIZE, log) != PAGE_SIZE)
            break;
        if (getU32(frame + 16) != checksum(frame, image))
            break;                              /* torn write: the log ends here */

        if (getU32(frame + 4)) {                /* a commit marker */
            lastGood   = frames;
            finalPages = getU32(frame + 8);
            finalRoot  = getU32(frame + 12);
        }
        frames++;
    }

    if (lastGood < ZERO) {                      /* nothing ever committed */
        fclose(log);
        remove(path);
        return SUCCESS_CODE;
    }

    /* A log may only be replayed into a file laid out in pages of the current
       version. An older layout keeps its metadata where a page write would
       land, so replaying into it would destroy it - as it once did. */
    if (!isPageFile(dbPath)) {
        fclose(log);
        remove(path);
        return SUCCESS_CODE;
    }

    FILE* db = fopen(dbPath, "r+b");
    if (db == NULL) {
        fclose(log);
        return ERROR_IO_CANNOT_OPEN;
    }

    if (fseek(log, WAL_HEADER, SEEK_SET) != ZERO) {
        fclose(log);
        fclose(db);
        return ERROR_IO_BAD_FORMAT;
    }

    for (long i = ZERO; i <= lastGood; i++) {
        if (fread(frame, ONE, FRAME_HEADER, log) != FRAME_HEADER
            || fread(image, ONE, PAGE_SIZE, log) != PAGE_SIZE) {
            fclose(log);
            fclose(db);
            return ERROR_IO_BAD_FORMAT;
        }

        long offset = (long)PAGE_SIZE * ((long)getU32(frame) + ONE);

        if (fseek(db, offset, SEEK_SET) != ZERO
            || fwrite(image, ONE, PAGE_SIZE, db) != PAGE_SIZE) {
            fclose(log);
            fclose(db);
            return ERROR_IO_WRITE;
        }
    }

    /* The header page records how many pages there are and where the catalog
       starts; both may have moved since the file was last written. */
    unsigned char dbHeader[PAGE_SIZE];

    if (fseek(db, ZERO, SEEK_SET) == ZERO
        && fread(dbHeader, ONE, PAGE_SIZE, db) == PAGE_SIZE) {
        putU32(dbHeader + 12, finalPages);
        putU32(dbHeader + 16, finalRoot);

        if (fseek(db, ZERO, SEEK_SET) != ZERO
            || fwrite(dbHeader, ONE, PAGE_SIZE, db) != PAGE_SIZE) {
            fclose(log);
            fclose(db);
            return ERROR_IO_WRITE;
        }
    }

    fflush(db);
    syncFile(db);
    fclose(db);
    fclose(log);
    remove(path);

    *recovered = (int)(lastGood + ONE);
    return SUCCESS_CODE;
}
