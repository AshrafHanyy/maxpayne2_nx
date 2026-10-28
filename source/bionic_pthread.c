/* bionic_pthread.c -- bionic-ABI pthread synchronization wrappers
 *
 * bionic's pthread structs have different sizes/layouts than newlib's, so we
 * store a pointer to a real object inside each and create it lazily (bionic
 * static initializers are all-zero, except recursive mutexes).
 *
 * bionic's structs are only 4-byte aligned (int32_t arrays), and 64-bit
 * atomics fault on unaligned addresses, so the pointer goes in the first
 * 8-byte aligned slot inside the object (offset 0 or 4; all are >= 16 bytes).
 *
 * Based on the approach in max_nx by fgsfds, Andy Nguyen (MIT).
 */

#include <switch.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "util.h"

#define BIONIC_ETIMEDOUT 110
#define BIONIC_EAGAIN 11

// bionic PTHREAD_RECURSIVE_MUTEX_INITIALIZER / PTHREAD_ERRORCHECK_MUTEX_INITIALIZER
#define BIONIC_MUTEX_INIT_RECURSIVE 0x4000
#define BIONIC_MUTEX_INIT_ERRORCHECK 0x8000
#define BIONIC_MUTEX_RECURSIVE 1

static inline int fix_err(int r) {
  return (r == ETIMEDOUT) ? BIONIC_ETIMEDOUT : r;
}

static inline uintptr_t *slot(void *obj) {
  return (uintptr_t *)(((uintptr_t)obj + 7) & ~(uintptr_t)7);
}

// mutex

static pthread_mutex_t *mutex_new(int recursive) {
  pthread_mutex_t *m = malloc(sizeof(*m));
  if (recursive)
    *m = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
  else
    *m = PTHREAD_MUTEX_INITIALIZER;
  return m;
}

static pthread_mutex_t *mutex_get(void *bm) {
  uintptr_t *s = slot(bm);
  uintptr_t v = __atomic_load_n(s, __ATOMIC_ACQUIRE);
  if (v > 0xFFFF) return (pthread_mutex_t *)v;
  // the static initializer's type bits live in the first int32
  const int init = *(volatile int32_t *)bm;
  pthread_mutex_t *m = mutex_new(init == BIONIC_MUTEX_INIT_RECURSIVE);
  if (__atomic_compare_exchange_n(s, &v, (uintptr_t)m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return m;
  free(m);
  return (pthread_mutex_t *)v;
}

int pthread_mutexattr_init_fake(long *attr) {
  *attr = 0;
  return 0;
}

int pthread_mutexattr_settype_fake(long *attr, int type) {
  *attr = type;
  return 0;
}

int pthread_mutexattr_destroy_fake(long *attr) {
  return 0;
}

int pthread_mutex_init_fake(void *bm, const long *attr) {
  *slot(bm) = (uintptr_t)mutex_new(attr && *attr == BIONIC_MUTEX_RECURSIVE);
  return 0;
}

int pthread_mutex_destroy_fake(void *bm) {
  uintptr_t *s = slot(bm);
  const uintptr_t v = *s;
  if (v > 0xFFFF) {
    free((void *)v);
    *s = 0;
  }
  return 0;
}

int pthread_mutex_lock_fake(void *bm) {
  return pthread_mutex_lock(mutex_get(bm));
}

int pthread_mutex_trylock_fake(void *bm) {
  return pthread_mutex_trylock(mutex_get(bm));
}

int pthread_mutex_unlock_fake(void *bm) {
  return pthread_mutex_unlock(mutex_get(bm));
}

// condition variable

static pthread_cond_t *cond_get(void *bc) {
  uintptr_t *s = slot(bc);
  uintptr_t v = __atomic_load_n(s, __ATOMIC_ACQUIRE);
  if (v) return (pthread_cond_t *)v;
  pthread_cond_t *c = malloc(sizeof(*c));
  pthread_cond_init(c, NULL);
  if (__atomic_compare_exchange_n(s, &v, (uintptr_t)c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return c;
  pthread_cond_destroy(c);
  free(c);
  return (pthread_cond_t *)v;
}

int pthread_cond_init_fake(void *bc, const void *attr) {
  pthread_cond_t *c = malloc(sizeof(*c));
  pthread_cond_init(c, NULL);
  *slot(bc) = (uintptr_t)c;
  return 0;
}

int pthread_cond_destroy_fake(void *bc) {
  uintptr_t *s = slot(bc);
  if (*s) {
    pthread_cond_destroy((pthread_cond_t *)*s);
    free((void *)*s);
    *s = 0;
  }
  return 0;
}

int pthread_cond_broadcast_fake(void *bc) {
  return pthread_cond_broadcast(cond_get(bc));
}

int pthread_cond_signal_fake(void *bc) {
  return pthread_cond_signal(cond_get(bc));
}

int pthread_cond_wait_fake(void *bc, void *bm) {
  return pthread_cond_wait(cond_get(bc), mutex_get(bm));
}

int pthread_cond_timedwait_fake(void *bc, void *bm, const struct timespec *t) {
  return fix_err(pthread_cond_timedwait(cond_get(bc), mutex_get(bm), t));
}

// rwlock (libnx RwLock)

static RwLock *rwlock_get(void *br) {
  uintptr_t *s = slot(br);
  uintptr_t v = __atomic_load_n(s, __ATOMIC_ACQUIRE);
  if (v) return (RwLock *)v;
  RwLock *l = malloc(sizeof(*l));
  rwlockInit(l);
  if (__atomic_compare_exchange_n(s, &v, (uintptr_t)l, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return l;
  free(l);
  return (RwLock *)v;
}

int pthread_rwlock_rdlock_fake(void *br) {
  rwlockReadLock(rwlock_get(br));
  return 0;
}

int pthread_rwlock_wrlock_fake(void *br) {
  rwlockWriteLock(rwlock_get(br));
  return 0;
}

int pthread_rwlock_unlock_fake(void *br) {
  RwLock *l = rwlock_get(br);
  if (rwlockIsWriteLockHeldByCurrentThread(l))
    rwlockWriteUnlock(l);
  else
    rwlockReadUnlock(l);
  return 0;
}

// semaphore

typedef struct {
  Mutex lock;
  CondVar cv;
  int count;
} fake_sem;

static fake_sem *sem_get(void *bs) {
  return (fake_sem *)*slot(bs);
}

int sem_init_fake(void *bs, int pshared, unsigned int value) {
  fake_sem *s = calloc(1, sizeof(*s));
  mutexInit(&s->lock);
  condvarInit(&s->cv);
  s->count = value;
  *slot(bs) = (uintptr_t)s;
  return 0;
}

int sem_destroy_fake(void *bs) {
  free(sem_get(bs));
  *slot(bs) = 0;
  return 0;
}

int sem_wait_fake(void *bs) {
  fake_sem *s = sem_get(bs);
  mutexLock(&s->lock);
  while (s->count <= 0)
    condvarWait(&s->cv, &s->lock);
  --s->count;
  mutexUnlock(&s->lock);
  return 0;
}

int sem_trywait_fake(void *bs) {
  fake_sem *s = sem_get(bs);
  int ret = 0;
  mutexLock(&s->lock);
  if (s->count > 0) {
    --s->count;
  } else {
    errno = EAGAIN;
    ret = -1;
  }
  mutexUnlock(&s->lock);
  return ret;
}

int sem_post_fake(void *bs) {
  fake_sem *s = sem_get(bs);
  mutexLock(&s->lock);
  ++s->count;
  condvarWakeOne(&s->cv);
  mutexUnlock(&s->lock);
  return 0;
}

int sem_getvalue_fake(void *bs, int *value) {
  fake_sem *s = sem_get(bs);
  mutexLock(&s->lock);
  *value = s->count;
  mutexUnlock(&s->lock);
  return 0;
}

// once (bionic pthread_once_t is an int, PTHREAD_ONCE_INIT == 0)

int pthread_once_fake(int *once, void (*init)(void)) {
  int expected = 0;
  if (__atomic_compare_exchange_n(once, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    init();
    __atomic_store_n(once, 2, __ATOMIC_RELEASE);
    return 0;
  }
  while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2)
    svcSleepThread(100000);
  return 0;
}
