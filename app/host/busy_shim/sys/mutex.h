/* host/busy_test: LV2 mutexes as pthread error-checking mutexes: as LV2, a non-recursive mutex refuses a second lock
 * by its owner (EDEADLK) and an unlock by a thread that does not own it (EPERM); every refusal is counted */
#pragma once
#include <pthread.h>
#include <stdlib.h>
extern int shim_mutex_errors;
typedef pthread_mutex_t *sys_mutex_t;
typedef struct { unsigned attr_protocol, attr_recursive, attr_pshared, attr_adaptive; unsigned long long key; int flags; char name[8]; } sys_mutex_attr_t;
#define sysMutexAttrInitialize(x) do { (x).attr_recursive = 0x20; (x).name[0] = 0; } while (0)
static inline int sysMutexCreate(sys_mutex_t *m, const sys_mutex_attr_t *at)
{
	pthread_mutexattr_t a;

	(void)at;
	*m = malloc(sizeof **m);
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
	return pthread_mutex_init(*m, &a);
}
static inline int sysMutexDestroy(sys_mutex_t m) { return pthread_mutex_destroy(m); }
static inline int sysMutexLock(sys_mutex_t m, unsigned long long t) { int r = pthread_mutex_lock(m); (void)t; if (r) shim_mutex_errors++; return r; }
static inline int sysMutexTryLock(sys_mutex_t m) { int r = pthread_mutex_trylock(m); if (r && r != 16) shim_mutex_errors++; return r; }	/* 16 EBUSY: just taken */
static inline int sysMutexUnlock(sys_mutex_t m) { int r = pthread_mutex_unlock(m); if (r) shim_mutex_errors++; return r; }
