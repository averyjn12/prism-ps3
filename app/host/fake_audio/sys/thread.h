#ifndef FAKE_THREAD_H
#define FAKE_THREAD_H
#include <ppu-types.h>
#include <pthread.h>
typedef pthread_t sys_ppu_thread_t;
#define THREAD_JOINABLE	1
s32 sysThreadCreate(sys_ppu_thread_t *th, void (*entry)(void *), void *arg, s32 prio, u64 stack, u64 flags, char *name);
s32 sysThreadJoin(sys_ppu_thread_t th, u64 *rv);
void sysThreadExit(u64 v);
#endif
