/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MBLUR_THREADPOOL_H
#define MBLUR_THREADPOOL_H

#include <stddef.h>

/*
 * A persistent pool. Threads are created once, with create(), and parked on
 * a condition variable between frames - spawning per frame would cost more
 * than the work itself at these frame times.
 */
typedef struct mblur_pool mblur_pool;
typedef void (*mblur_pool_fn)(void *user, size_t begin, size_t end,
                              unsigned worker);

/* `threads` counts workers including the calling thread. 0 asks the pool to
 * choose; 1 returns NULL, meaning "run inline", which is not an error. */
mblur_pool *mblur_pool_create(unsigned threads);
void mblur_pool_destroy(mblur_pool *p);
unsigned mblur_pool_threads(const mblur_pool *p);

/*
 * Splits [0, count) into contiguous chunks and runs fn over them, returning
 * once every chunk is done. The calling thread takes a chunk too, so a
 * 4-thread pool means 3 workers plus this one.
 */
void mblur_pool_run(mblur_pool *p, size_t count, mblur_pool_fn fn, void *user);

unsigned mblur_hardware_threads(void);

#endif /* MBLUR_THREADPOOL_H */
