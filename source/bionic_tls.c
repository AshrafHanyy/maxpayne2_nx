/* bionic_tls.c -- per-thread state expected by Android-compiled code
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "bionic_tls.h"
#include "util.h"

#define TLS_SLOT_STACK_GUARD 5
#define STACK_GUARD_VALUE ((void *)0x4242424242424242ULL)
#define DEFAULT_STACK_SIZE (1024 * 1024)

static bionic_tls s_main_tls;
static bionic_thread s_main_thread;

static Mutex s_key_lock;
static uint8_t s_key_used[BIONIC_MAX_KEYS];
static void (*s_key_dtor[BIONIC_MAX_KEYS])(void *);

static Mutex s_emutls_lock;
static uintptr_t s_emutls_next = 0;

static bionic_tls *tls_new(void) {
  bionic_tls *t = calloc(1, sizeof(*t));
  t->slots[TLS_SLOT_STACK_GUARD] = STACK_GUARD_VALUE;
  return t;
}

void tls_init_main(void) {
  s_main_tls.slots[TLS_SLOT_STACK_GUARD] = STACK_GUARD_VALUE;
  s_main_thread.real = pthread_self();
  s_main_thread.tls = &s_main_tls;
  s_main_tls.self = &s_main_thread;
  armSetTlsRw(&s_main_tls);
}

// threads we didn't create (audio, Mesa) get a block on first use
bionic_tls *tls_current(void) {
  bionic_tls *t = armGetTlsRw();
  if (!t) {
    t = tls_new();
    t->self = calloc(1, sizeof(bionic_thread));
    t->self->real = pthread_self();
    t->self->tls = t;
    armSetTlsRw(t);
  }
  return t;
}

static void tls_thread_exit(bionic_tls *t) {
  // run key destructors like bionic does, up to 4 passes
  for (int pass = 0; pass < 4; ++pass) {
    int any = 0;
    for (int k = 0; k < BIONIC_MAX_KEYS; ++k) {
      void *v = t->key_values[k];
      if (v && s_key_used[k] && s_key_dtor[k]) {
        t->key_values[k] = NULL;
        s_key_dtor[k](v);
        any = 1;
      }
    }
    if (!any) break;
  }
  for (size_t i = 0; i < t->emutls_count; ++i)
    free(t->emutls[i]);
  free(t->emutls);
  t->emutls = NULL;
  t->emutls_count = 0;
}

int pthread_key_create_fake(int *key, void (*dtor)(void *)) {
  mutexLock(&s_key_lock);
  for (int k = 0; k < BIONIC_MAX_KEYS; ++k) {
    if (!s_key_used[k]) {
      s_key_used[k] = 1;
      s_key_dtor[k] = dtor;
      mutexUnlock(&s_key_lock);
      *key = k;
      return 0;
    }
  }
  mutexUnlock(&s_key_lock);
  debugPrintf("pthread_key_create: out of keys\n");
  return EAGAIN;
}

int pthread_key_delete_fake(int key) {
  if (key < 0 || key >= BIONIC_MAX_KEYS) return EINVAL;
  mutexLock(&s_key_lock);
  s_key_used[key] = 0;
  s_key_dtor[key] = NULL;
  mutexUnlock(&s_key_lock);
  return 0;
}

void *pthread_getspecific_fake(int key) {
  if (key < 0 || key >= BIONIC_MAX_KEYS) return NULL;
  return tls_current()->key_values[key];
}

int pthread_setspecific_fake(int key, const void *value) {
  if (key < 0 || key >= BIONIC_MAX_KEYS) return EINVAL;
  tls_current()->key_values[key] = (void *)value;
  return 0;
}

// matches compiler-rt's __emutls_control
typedef struct {
  size_t size;
  size_t align;
  union {
    uintptr_t index;
    void *address;
  } object;
  void *value;
} emutls_control;

void *__emutls_get_address_fake(void *control) {
  emutls_control *c = control;
  uintptr_t idx = __atomic_load_n(&c->object.index, __ATOMIC_ACQUIRE);
  if (!idx) {
    mutexLock(&s_emutls_lock);
    idx = c->object.index;
    if (!idx) {
      idx = ++s_emutls_next;
      __atomic_store_n(&c->object.index, idx, __ATOMIC_RELEASE);
    }
    mutexUnlock(&s_emutls_lock);
  }

  bionic_tls *t = tls_current();
  if (idx > t->emutls_count) {
    const size_t new_count = idx + 16;
    t->emutls = realloc(t->emutls, new_count * sizeof(void *));
    memset(t->emutls + t->emutls_count, 0, (new_count - t->emutls_count) * sizeof(void *));
    t->emutls_count = new_count;
  }

  void **slot = &t->emutls[idx - 1];
  if (!*slot) {
    const size_t align = c->align < sizeof(void *) ? sizeof(void *) : c->align;
    void *p = aligned_alloc(align, (c->size + align - 1) & ~(align - 1));
    if (c->value) memcpy(p, c->value, c->size);
    else memset(p, 0, c->size);
    *slot = p;
  }
  return *slot;
}

// threads

static void *thread_trampoline(void *arg) {
  bionic_thread *th = arg;
  armSetTlsRw(th->tls);
  // libnx starts every thread on the default core; spread the game's threads out
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, th->core, 1u << th->core);
  void *ret = th->entry(th->arg);
  tls_thread_exit(th->tls);
  return ret;
}

// matches bionic's pthread_attr_t on LP64
typedef struct {
  uint32_t flags;
  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
  char reserved[16];
} bionic_attr;

static int create_thread(long *thread, const void *attr, void *(*entry)(void *), void *arg, int core) {
  const bionic_attr *battr = attr;
  bionic_thread *th = calloc(1, sizeof(*th));
  th->entry = entry;
  th->arg = arg;
  th->core = core;
  th->tls = tls_new();
  th->tls->self = th;

  size_t stack_size = (battr && battr->stack_size) ? battr->stack_size : DEFAULT_STACK_SIZE;
  if (stack_size < DEFAULT_STACK_SIZE) stack_size = DEFAULT_STACK_SIZE;

  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, stack_size);
  const int ret = pthread_create(&th->real, &a, thread_trampoline, th);
  pthread_attr_destroy(&a);
  if (ret) {
    debugPrintf("pthread_create failed: %d\n", ret);
    free(th->tls);
    free(th);
    return ret;
  }
  *thread = (long)th;
  return 0;
}

int pthread_create_fake(long *thread, const void *attr, void *(*entry)(void *), void *arg) {
  return create_thread(thread, attr, entry, arg, CORE_GAME_WORKERS);
}

int tls_spawn_thread(long *thread, void *(*entry)(void *), void *arg, size_t stack_size, int core) {
  bionic_attr attr;
  pthread_attr_init_fake(&attr);
  attr.stack_size = stack_size;
  return create_thread(thread, &attr, entry, arg, core);
}

int pthread_join_fake(long thread, void **ret) {
  bionic_thread *th = (bionic_thread *)thread;
  const int r = pthread_join(th->real, ret);
  if (r == 0) {
    free(th->tls);
    free(th);
  }
  return r;
}

int pthread_detach_fake(long thread) {
  bionic_thread *th = (bionic_thread *)thread;
  return pthread_detach(th->real);
}

long pthread_self_fake(void) {
  return (long)tls_current()->self;
}

int pthread_attr_init_fake(void *attr) {
  bionic_attr *a = attr;
  memset(a, 0, sizeof(*a));
  a->stack_size = DEFAULT_STACK_SIZE;
  a->guard_size = 0x1000;
  return 0;
}

int pthread_attr_destroy_fake(void *attr) {
  return 0;
}

int pthread_attr_getstacksize_fake(const void *attr, size_t *size) {
  *size = ((const bionic_attr *)attr)->stack_size;
  return 0;
}

int pthread_attr_getschedparam_fake(const void *attr, int *param) {
  *param = ((const bionic_attr *)attr)->sched_priority;
  return 0;
}

int pthread_attr_setschedparam_fake(void *attr, const int *param) {
  ((bionic_attr *)attr)->sched_priority = *param;
  return 0;
}
