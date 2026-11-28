/* host/busy_test: LV2 threads as pthreads */
#pragma once
#include <pthread.h>
#include <stdlib.h>
typedef pthread_t sys_ppu_thread_t;
#define THREAD_JOINABLE 1
struct shim_tramp { void (*e)(void *); void *a; };
static void *shim_run(void *p) { struct shim_tramp t = *(struct shim_tramp *)p; free(p); t.e(t.a); return NULL; }
static inline int sysThreadCreate(sys_ppu_thread_t *th, void (*e)(void *), void *a, int prio, unsigned long long ss,
	unsigned long long fl, char *name)
{
	pthread_attr_t at;
	struct shim_tramp *t = malloc(sizeof *t);

	(void)prio; (void)fl; (void)name;
	t->e = e;
	t->a = a;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, ss);
	return pthread_create(th, &at, shim_run, t);
}
static inline void sysThreadExit(unsigned long long v) { (void)v; pthread_exit(NULL); }
