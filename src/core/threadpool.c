/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "threadpool.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION mb_mutex;
typedef CONDITION_VARIABLE mb_cond;
typedef HANDLE mb_thread;
#define MB_MUTEX_INIT(m) InitializeCriticalSection(m)
#define MB_MUTEX_FREE(m) DeleteCriticalSection(m)
#define MB_LOCK(m) EnterCriticalSection(m)
#define MB_UNLOCK(m) LeaveCriticalSection(m)
#define MB_COND_INIT(c) InitializeConditionVariable(c)
#define MB_COND_FREE(c) ((void)0)
#define MB_WAIT(c, m) SleepConditionVariableCS(c, m, INFINITE)
#define MB_SIGNAL_ALL(c) WakeAllConditionVariable(c)
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t mb_mutex;
typedef pthread_cond_t mb_cond;
typedef pthread_t mb_thread;
#define MB_MUTEX_INIT(m) pthread_mutex_init(m, NULL)
#define MB_MUTEX_FREE(m) pthread_mutex_destroy(m)
#define MB_LOCK(m) pthread_mutex_lock(m)
#define MB_UNLOCK(m) pthread_mutex_unlock(m)
#define MB_COND_INIT(c) pthread_cond_init(c, NULL)
#define MB_COND_FREE(c) pthread_cond_destroy(c)
#define MB_WAIT(c, m) pthread_cond_wait(c, m)
#define MB_SIGNAL_ALL(c) pthread_cond_broadcast(c)
#endif

struct mblur_pool {
    unsigned threads;   /* total participants, including the caller */
    unsigned workers;   /* spawned threads = threads - 1 */
    mb_thread *tid;

    mb_mutex lock;
    mb_cond work_ready;
    mb_cond work_done;

    mblur_pool_fn fn;
    void *user;
    size_t count;
    uint_least64_t generation;
    unsigned outstanding;
    int shutdown;
};

unsigned mblur_hardware_threads(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? (unsigned)si.dwNumberOfProcessors : 1u;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (unsigned)n : 1u;
#endif
}

/* Chunk k of `count` items across `threads` participants, balanced so the
 * remainder is spread one item at a time rather than dumped on the last. */
static void chunk_range(size_t count, unsigned threads, unsigned k,
                        size_t *begin, size_t *end)
{
    const size_t base = count / threads;
    const size_t rem = count % threads;
    const size_t b = base * k + ((k < rem) ? k : rem);
    const size_t n = base + ((k < rem) ? 1u : 0u);
    *begin = b;
    *end = b + n;
}

#if defined(_WIN32)
static DWORD WINAPI worker_main(LPVOID arg)
#else
static void *worker_main(void *arg)
#endif
{
    struct {
        mblur_pool *pool;
        unsigned index;
    } *self = arg;
    mblur_pool *p = self->pool;
    const unsigned index = self->index;
    free(self);

    uint_least64_t seen = 0;

    MB_LOCK(&p->lock);
    for (;;) {
        while (!p->shutdown && p->generation == seen)
            MB_WAIT(&p->work_ready, &p->lock);
        if (p->shutdown)
            break;

        seen = p->generation;
        mblur_pool_fn fn = p->fn;
        void *user = p->user;
        size_t count = p->count;
        unsigned threads = p->threads;
        MB_UNLOCK(&p->lock);

        size_t b, e;
        chunk_range(count, threads, index, &b, &e);
        if (e > b)
            fn(user, b, e, index);

        MB_LOCK(&p->lock);
        if (--p->outstanding == 0)
            MB_SIGNAL_ALL(&p->work_done);
    }
    MB_UNLOCK(&p->lock);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

mblur_pool *mblur_pool_create(unsigned threads)
{
    if (threads == 0)
        threads = mblur_hardware_threads();
    if (threads <= 1)
        return NULL; /* run inline; not a failure */

    mblur_pool *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    p->threads = threads;
    p->workers = threads - 1;
    p->tid = calloc(p->workers, sizeof(*p->tid));
    if (!p->tid) {
        free(p);
        return NULL;
    }

    MB_MUTEX_INIT(&p->lock);
    MB_COND_INIT(&p->work_ready);
    MB_COND_INIT(&p->work_done);

    for (unsigned i = 0; i < p->workers; i++) {
        struct {
            mblur_pool *pool;
            unsigned index;
        } *arg = malloc(sizeof(*arg));
        if (!arg) {
            p->workers = i;
            break;
        }
        arg->pool = p;
        arg->index = i + 1; /* index 0 belongs to the calling thread */
#if defined(_WIN32)
        p->tid[i] = CreateThread(NULL, 0, worker_main, arg, 0, NULL);
        if (!p->tid[i]) {
            free(arg);
            p->workers = i;
            break;
        }
#else
        if (pthread_create(&p->tid[i], NULL, worker_main, arg) != 0) {
            free(arg);
            p->workers = i;
            break;
        }
#endif
    }

    p->threads = p->workers + 1;
    if (p->threads <= 1) {
        mblur_pool_destroy(p);
        return NULL;
    }
    return p;
}

void mblur_pool_destroy(mblur_pool *p)
{
    if (!p)
        return;

    MB_LOCK(&p->lock);
    p->shutdown = 1;
    MB_SIGNAL_ALL(&p->work_ready);
    MB_UNLOCK(&p->lock);

    for (unsigned i = 0; i < p->workers; i++) {
#if defined(_WIN32)
        WaitForSingleObject(p->tid[i], INFINITE);
        CloseHandle(p->tid[i]);
#else
        pthread_join(p->tid[i], NULL);
#endif
    }

    MB_COND_FREE(&p->work_done);
    MB_COND_FREE(&p->work_ready);
    MB_MUTEX_FREE(&p->lock);
    free(p->tid);
    free(p);
}

unsigned mblur_pool_threads(const mblur_pool *p)
{
    return p ? p->threads : 1u;
}

void mblur_pool_run(mblur_pool *p, size_t count, mblur_pool_fn fn, void *user)
{
    if (count == 0)
        return;

    if (!p) {
        fn(user, 0, count, 0);
        return;
    }

    MB_LOCK(&p->lock);
    p->fn = fn;
    p->user = user;
    p->count = count;
    p->outstanding = p->workers;
    p->generation++;
    MB_SIGNAL_ALL(&p->work_ready);
    MB_UNLOCK(&p->lock);

    size_t b, e;
    chunk_range(count, p->threads, 0, &b, &e);
    if (e > b)
        fn(user, b, e, 0);

    MB_LOCK(&p->lock);
    while (p->outstanding != 0)
        MB_WAIT(&p->work_done, &p->lock);
    MB_UNLOCK(&p->lock);
}
