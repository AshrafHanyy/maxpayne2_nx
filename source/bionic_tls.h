/* bionic_tls.h -- per-thread state expected by Android-compiled code
 *
 * Android code reads its stack protector cookie from TPIDR_EL0 + 0x28
 * (bionic TLS slot 5). Every thread that runs game code needs a block
 * there. We also keep pthread keys and emulated TLS in the same block.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __BIONIC_TLS_H__
#define __BIONIC_TLS_H__

#include <stddef.h>
#include <pthread.h>

#define BIONIC_TLS_SLOTS 16
#define BIONIC_MAX_KEYS 128

typedef struct bionic_thread bionic_thread;

typedef struct bionic_tls {
  void *slots[BIONIC_TLS_SLOTS]; // TPIDR_EL0 points here
  void *key_values[BIONIC_MAX_KEYS];
  void **emutls;
  size_t emutls_count;
  bionic_thread *self;
} bionic_tls;

struct bionic_thread {
  pthread_t real;
  void *(*entry)(void *);
  void *arg;
  bionic_tls *tls;
  int core;
};

// CPU cores: audio (SDL/openal threads) stays on 0, the game's render thread
// runs on 1 and the game's worker threads (loading, streaming) on 2
#define CORE_GAME_RENDER 1
#define CORE_GAME_WORKERS 2

void tls_init_main(void);
bionic_tls *tls_current(void);
// starts a thread that can run game code (has a bionic TLS block)
int tls_spawn_thread(long *thread, void *(*entry)(void *), void *arg, size_t stack_size, int core);

// bionic ABI replacements
int pthread_key_create_fake(int *key, void (*dtor)(void *));
int pthread_key_delete_fake(int key);
void *pthread_getspecific_fake(int key);
int pthread_setspecific_fake(int key, const void *value);
void *__emutls_get_address_fake(void *control);

int pthread_create_fake(long *thread, const void *attr, void *(*entry)(void *), void *arg);
int pthread_join_fake(long thread, void **ret);
int pthread_detach_fake(long thread);
long pthread_self_fake(void);

int pthread_attr_init_fake(void *attr);
int pthread_attr_destroy_fake(void *attr);
int pthread_attr_getstacksize_fake(const void *attr, size_t *size);
int pthread_attr_getschedparam_fake(const void *attr, int *param);
int pthread_attr_setschedparam_fake(void *attr, const int *param);

#endif
