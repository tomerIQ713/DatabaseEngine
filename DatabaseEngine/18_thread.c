/* Before <pthread.h>: PTHREAD_MUTEX_RECURSIVE is XSI, and -std=c17 hides it.
   The same reasoning as the block at the top of sql_common.h, repeated here
   because this file reaches a system header before that one. */
#ifndef _WIN32
#  ifdef __APPLE__
#    define _DARWIN_C_SOURCE 1
#  else
#    define _XOPEN_SOURCE 700
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#ifdef _WIN32
/* SRWLOCK arrived in Vista, and MinGW still defaults its headers to something
   older - without this the lock functions are simply not declared. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
/* winnt.h has an enumerator called TokenType, and so does this engine - the
   same collision 16_wire.c and 12_persist.c work around. */
#define TokenType WindowsTokenType
#include <windows.h>
#undef TokenType
#else
#include <pthread.h>
#endif

#include "sql_common.h"

typedef struct Mutex  Mutex;
typedef struct RwLock RwLock;

/*
 * Threads, a mutex, and a reader-writer lock, in the two spellings this engine
 * is built with. Nothing here is clever: it is the smallest surface the rest of
 * the code needs, so that no other file has to know which platform it is on.
 *
 * The reader-writer lock is the one that matters. Statements that only read -
 * SELECT, and the dot-commands that report - take it shared and genuinely run
 * at the same time; anything that writes takes it exclusive and runs alone.
 * That is the whole of the concurrency model, and it is deliberately coarse:
 * one lock, held for one statement, with no lock ordering to get wrong because
 * there is only ever one to hold.
 *
 * ponytail: SRWLOCK and pthread_rwlock rather than something hand-rolled.
 * Neither is fair - a stream of readers can in principle starve a writer on
 * some platforms - and the upgrade if that ever shows up is a queued lock of
 * our own, which is exactly the kind of thing not to write until it is needed.
 */

#ifdef _WIN32

struct Mutex  { CRITICAL_SECTION handle; };
struct RwLock { SRWLOCK          handle; };

/* A thread entry point has a different signature here than on POSIX, so the
   two are bridged by a real function rather than a cast between incompatible
   function types - which is undefined behaviour, and which -Wextra says so. */
typedef struct {
    ThreadFunction function;
    void*          argument;
} Launch;

static DWORD WINAPI runThread(LPVOID raw)
{
    Launch*        launch   = (Launch*)raw;
    ThreadFunction function = launch->function;
    void*          argument = launch->argument;

    free(launch);
    function(argument);
    return ZERO;
}

int threadStart(ThreadFunction function, void* argument)
{
    Launch* launch = (Launch*)malloc(sizeof(Launch));

    if (launch == NULL)
        return ERROR_EXEC_OUT_OF_MEMORY;

    launch->function = function;
    launch->argument = argument;

    HANDLE handle = CreateThread(NULL, ZERO, runThread, launch, ZERO, NULL);

    if (handle == NULL) {
        free(launch);
        return ERROR_EXEC_OUT_OF_MEMORY;
    }

    /* Nothing ever joins a connection thread: it ends when its client goes
       away, and the handle would otherwise leak for the life of the server. */
    CloseHandle(handle);
    return SUCCESS_CODE;
}

void mutexInit(Mutex* lock)    { InitializeCriticalSection(&lock->handle); }
void mutexFree(Mutex* lock)    { DeleteCriticalSection(&lock->handle); }
void mutexLock(Mutex* lock)    { EnterCriticalSection(&lock->handle); }
void mutexUnlock(Mutex* lock)  { LeaveCriticalSection(&lock->handle); }

void rwInit(RwLock* lock)      { InitializeSRWLock(&lock->handle); }
void rwFree(RwLock* lock)      { (void)lock; }          /* SRWLOCK needs none */
void rwReadLock(RwLock* lock)  { AcquireSRWLockShared(&lock->handle); }
void rwWriteLock(RwLock* lock) { AcquireSRWLockExclusive(&lock->handle); }
void rwReadUnlock(RwLock* lock)  { ReleaseSRWLockShared(&lock->handle); }
void rwWriteUnlock(RwLock* lock) { ReleaseSRWLockExclusive(&lock->handle); }

#else

struct Mutex  { pthread_mutex_t  handle; };
struct RwLock { pthread_rwlock_t handle; };

int threadStart(ThreadFunction function, void* argument)
{
    pthread_t     thread;
    pthread_attr_t attributes;

    if (pthread_attr_init(&attributes) != ZERO)
        return ERROR_EXEC_OUT_OF_MEMORY;

    /* Detached for the same reason CloseHandle is called above: no one joins. */
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);

    int failed = pthread_create(&thread, &attributes,
                                (void* (*)(void*))function, argument);

    pthread_attr_destroy(&attributes);
    return failed ? ERROR_EXEC_OUT_OF_MEMORY : SUCCESS_CODE;
}

/* Recursive, to match the CRITICAL_SECTION above: the pool calls its own
   entry points in places, and a self-deadlock there would be a hang with no
   obvious cause. */
void mutexInit(Mutex* lock)
{
    pthread_mutexattr_t attributes;

    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock->handle, &attributes);
    pthread_mutexattr_destroy(&attributes);
}
void mutexFree(Mutex* lock)    { pthread_mutex_destroy(&lock->handle); }
void mutexLock(Mutex* lock)    { pthread_mutex_lock(&lock->handle); }
void mutexUnlock(Mutex* lock)  { pthread_mutex_unlock(&lock->handle); }

void rwInit(RwLock* lock)      { pthread_rwlock_init(&lock->handle, NULL); }
void rwFree(RwLock* lock)      { pthread_rwlock_destroy(&lock->handle); }
void rwReadLock(RwLock* lock)  { pthread_rwlock_rdlock(&lock->handle); }
void rwWriteLock(RwLock* lock) { pthread_rwlock_wrlock(&lock->handle); }
void rwReadUnlock(RwLock* lock)  { pthread_rwlock_unlock(&lock->handle); }
void rwWriteUnlock(RwLock* lock) { pthread_rwlock_unlock(&lock->handle); }

#endif

/*
 * The engine lock, and the pool's own.
 *
 * Two locks, and they nest in one direction only: a statement takes the engine
 * lock, and the pool takes its mutex underneath. Nothing ever takes them the
 * other way round, which is why there is no ordering rule to remember.
 */
/* Set once a server starts a thread. Until then every lock here is a
   no-op, so a plain REPL session pays nothing for any of it. */
static int started;

/*
 * How many connections are being served. The accept loop adds and each
 * connection thread removes, so it needs a lock of its own - it is not the
 * engine's data and must not wait behind a statement.
 */
static Mutex countLock;
static int   sessionCount;

int sessionAcquire(int limit)
{
    int room;

    if (!started)
        return ONE;

    mutexLock(&countLock);

    room = sessionCount < limit;
    if (room)
        sessionCount++;

    mutexUnlock(&countLock);
    return room;
}

void sessionRelease(void)
{
    if (!started)
        return;

    mutexLock(&countLock);
    sessionCount--;
    mutexUnlock(&countLock);
}

static RwLock engineLock;
static Mutex  poolLock;

void initThreads(void)
{
    if (started)
        return;

    rwInit(&engineLock);
    mutexInit(&poolLock);
    mutexInit(&countLock);
    sessionCount = ZERO;
    started      = ONE;
}

void freeThreads(void)
{
    if (!started)
        return;

    rwFree(&engineLock);
    mutexFree(&poolLock);
    mutexFree(&countLock);
    started = ZERO;
}

/* Single-threaded until the server starts one: the REPL never calls
   initThreads, so these are all no-ops in a plain session. */
void engineReadLock(void)    { if (started) rwReadLock(&engineLock); }
void engineWriteLock(void)   { if (started) rwWriteLock(&engineLock); }
void engineReadUnlock(void)  { if (started) rwReadUnlock(&engineLock); }
void engineWriteUnlock(void) { if (started) rwWriteUnlock(&engineLock); }

void poolEnter(void) { if (started) mutexLock(&poolLock); }
void poolLeave(void) { if (started) mutexUnlock(&poolLock); }
