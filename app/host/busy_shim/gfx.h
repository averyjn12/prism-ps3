#pragma once
typedef unsigned long long u64;
extern volatile int gx_lent;
extern void (*gx_take_back)(void);
extern int gfx_gpu, gfx_dead;
