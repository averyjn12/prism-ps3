/* 1.00 (Bananafuzzbuddy, 2026-11-27: "when we are at loading screens in the prism app, i still need the background to move
 * slowly so I know I'm not frozen"): the loading screen keeps moving while the app waits.
 *
 * A loading screen (screens.c progress) is drawn once per call, and the app then often waits seconds in a call that
 * draws nothing: a card read, webMAN's mount, the wait for /dev_bdvd, a copy's last write. This thread redraws the
 * last loading screen about 30 times a second in those waits, from a copy of what it showed.
 *
 * Who draws: one mutex. The main thread holds it always, except after a loading screen, when it LENDS the drawing
 * (busy_lend: gx_lent = 1, unlock). The ticker draws only while it holds the mutex. Every way into the RSX drawing
 * (gfx.c GX_TAKE: gx_prims, gx_clear, gx_depth, gx_scissor, gx_font, gx_tex_forget, gfx_present, gfx_end) first takes
 * the drawing back: it locks the mutex, which waits out the ticker's frame, so the main thread never draws into a
 * frame the ticker has begun. The ticker's own calls come through the same doors; they are told apart by the stack
 * they run on (its own, measured when it starts).
 * Only with the RSX drawing (gfx_gpu): the CPU drawing writes the frame from ui.c directly, past those doors. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include "gfx.h"
#include "games.h"
#include "screens.h"
#include "busy.h"

#define TB_HZ		79800000ULL
#define BUSY_STACK	0x10000

static sys_mutex_t mx;
static sys_ppu_thread_t th;
static int ok;				/* the mutex, held by the main thread, and the ticker exist */
static volatile int run;
static volatile uintptr_t tk_lo, tk_hi;	/* the ticker's stack */
static volatile u64 s_last;		/* when the loading screen was last drawn */
static game_t s_game;
static int s_has_game;
static char s_head[48], s_title[160], s_what[200];
static float s_done;
unsigned busy_frames;			/* frames the ticker drew (the perf log) */

static u64 tbr(void)
{
	u64 t;

	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

static int on_ticker(void)
{
	uintptr_t a = (uintptr_t)__builtin_frame_address(0);

	return a >= tk_lo && a < tk_hi;
}

static void take_back(void)
{
	if (!ok || !gx_lent || on_ticker())
		return;
	sysMutexLock(mx, 0);
	gx_lent = 0;
}

static void ticker(void *arg)
{
	volatile char here = 0;

	(void)arg;
	tk_hi = (uintptr_t)&here + 1024;
	tk_lo = tk_hi - BUSY_STACK - 1024;
	while (run)
	{
		usleep(16000);
		if (!gx_lent || sysMutexTryLock(mx) != 0)
			continue;
		if (gx_lent && tbr() - s_last > TB_HZ / 40)	/* ~30 frames a second */
		{
			progress_draw(s_has_game ? &s_game : NULL, s_head, s_title, s_done, s_what);
			s_last = tbr();
			busy_frames++;
		}
		sysMutexUnlock(mx);
	}
	sysThreadExit(0);
}

int busy_init(void)
{
	sys_mutex_attr_t at;

	if (ok || !gfx_gpu)
		return -1;
	memset(&at, 0, sizeof at);
	sysMutexAttrInitialize(at);
	if (sysMutexCreate(&mx, &at) != 0)
		return -2;
	if (sysMutexLock(mx, 0) != 0)
	{
		sysMutexDestroy(mx);
		return -3;
	}
	run = 1;
	/* priority 1500: below the main thread's (1000) and the sound's (500), so it only draws while they wait */
	if (sysThreadCreate(&th, ticker, NULL, 1500, BUSY_STACK, THREAD_JOINABLE, (char *)"prism_busy") != 0)
	{
		run = 0;
		sysMutexUnlock(mx);
		sysMutexDestroy(mx);
		return -4;
	}
	gx_take_back = take_back;
	ok = 1;
	return 0;
}

/* the main thread, before drawing a loading screen: the drawing back, and a copy of what the screen shows */
void busy_save(const void *g, const char *head, const char *title, float done, const char *what)
{
	take_back();
	if (!ok)
		return;
	s_has_game = g != NULL;
	if (g)
		memcpy(&s_game, g, sizeof s_game);
	snprintf(s_head, sizeof s_head, "%s", head ? head : "");
	snprintf(s_title, sizeof s_title, "%s", title ? title : "");
	snprintf(s_what, sizeof s_what, "%s", what ? what : "");
	s_done = done;
}

/* the main thread, after drawing a loading screen: lend the drawing until its next draw */
void busy_lend(void)
{
	if (!ok || gx_lent || !gfx_gpu || gfx_dead)
		return;
	s_last = tbr();
	gx_lent = 1;
	sysMutexUnlock(mx);
}
