/* pthread.h - the slice of POSIX threads the engine uses, over Win32 primitives.
 *
 * Mutexes are SRW locks and conditions are CONDITION_VARIABLEs: both are a single pointer
 * whose zero value is the initialised state, which is what lets PTHREAD_MUTEX_INITIALIZER
 * stay a static initialiser. The engine's own worker pool (qwen_tts_thread.c) already has a
 * Win32 backend; this covers the mutexes and the one-off threads elsewhere. The shapes
 * below hold no Win32 types so that <windows.h> stays out of the engine's translation
 * units; the implementation in qwen_msvc_compat.c casts. */
#ifndef QWEN_MSVC_PTHREAD_H
#define QWEN_MSVC_PTHREAD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { void *handle; } pthread_mutex_t;
typedef struct { void *handle; } pthread_cond_t;
typedef struct { void *handle; } pthread_once_t;
typedef struct qwen_msvc_thread *pthread_t;
typedef int pthread_attr_t;
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER {0}
#define PTHREAD_COND_INITIALIZER {0}
#define PTHREAD_ONCE_INIT {0}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
int pthread_detach(pthread_t t);
pthread_t pthread_self(void);
int pthread_once(pthread_once_t *once, void (*fn)(void));
int pthread_setname_np(const char *name);

int sched_yield(void);

#ifdef __cplusplus
}
#endif

#endif
