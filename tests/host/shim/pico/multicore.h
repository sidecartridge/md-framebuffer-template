/* Host stand-in for the Pico SDK's pico/multicore.h: core 1 is a thread,
 * and the two inter-core FIFOs are blocking queues, so code that hands work
 * to core 1 runs on two threads here as on the two cores of the RP.
 * Include it from one translation unit only (it holds the queues). */
#ifndef HOST_SHIM_PICO_MULTICORE_H
#define HOST_SHIM_PICO_MULTICORE_H

#include <pthread.h>
#include <stdint.h>

#include "pico/stdlib.h"

typedef struct {
  uint32_t items[64];
  unsigned head, count;
  pthread_mutex_t lock;
  pthread_cond_t changed;
} host_fifo_t;

/* host_fifo[n]: what core n reads. */
static host_fifo_t host_fifo[2] = {
    {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER},
    {.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER}};
static _Thread_local int host_core_num; /* 0 on the main thread */
static void (*host_core1_entry)(void);

static inline void host_fifo_push(host_fifo_t *f, uint32_t value) {
  pthread_mutex_lock(&f->lock);
  while (f->count == sizeof f->items / sizeof f->items[0]) {
    pthread_cond_wait(&f->changed, &f->lock);
  }
  f->items[(f->head + f->count++) % (sizeof f->items / sizeof f->items[0])] = value;
  pthread_cond_broadcast(&f->changed);
  pthread_mutex_unlock(&f->lock);
}

static inline uint32_t host_fifo_pop(host_fifo_t *f) {
  pthread_mutex_lock(&f->lock);
  while (f->count == 0) {
    pthread_cond_wait(&f->changed, &f->lock);
  }
  uint32_t value = f->items[f->head];
  f->head = (f->head + 1) % (sizeof f->items / sizeof f->items[0]);
  f->count--;
  pthread_cond_broadcast(&f->changed);
  pthread_mutex_unlock(&f->lock);
  return value;
}

static inline void multicore_fifo_push_blocking(uint32_t value) {
  host_fifo_push(&host_fifo[1 - host_core_num], value);
}

/* The RP's code passes pointers (a job, its argument) through the 32-bit
 * FIFO. On a 64-bit host that keeps only their low half; every such pointer
 * points into this program's own image, so the high half is taken from an
 * address in it. Values that are not pointers are unaffected in their low
 * 32 bits, which is all the RP's code keeps of them. */
static inline uintptr_t multicore_fifo_pop_blocking(void) {
  uint32_t low = host_fifo_pop(&host_fifo[host_core_num]);
  return ((uintptr_t)&host_fifo & ~(uintptr_t)0xFFFFFFFFu) | low;
}

#define multicore_fifo_push_blocking_inline multicore_fifo_push_blocking
#define multicore_fifo_pop_blocking_inline multicore_fifo_pop_blocking

static void *host_core1_main(void *unused) {
  (void)unused;
  host_core_num = 1;
  host_core1_entry();
  return NULL;
}

static inline void multicore_launch_core1(void (*entry)(void)) {
  pthread_t thread;
  host_core1_entry = entry;
  pthread_create(&thread, NULL, host_core1_main, NULL);
  pthread_detach(thread);
}

#endif
