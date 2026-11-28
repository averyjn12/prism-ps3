/* PC test of src/busy.c (1.00, the loading-screen ticker), with real threads: host/busy_test.py builds a COPY of
 * busy.c beside the stand-in headers in host/busy_shim (sys/thread.h, sys/mutex.h -> pthreads with LV2's rules: a
 * non-recursive mutex unlocked only by its owner, a second lock by the owner refused), the PPU's mftb replaced by
 * the host clock.
 *
 * The drawing stand-in models what matters on the console: a frame is every drawing call from one present to the
 * next, and it must come from ONE thread (two threads queuing into one RSX frame mixes their triangles). Every
 * draw call takes the drawing back first (gfx.c GX_TAKE). Draws are slowed so a race has room to happen.
 *
 * Checks (stdout "PASS"/"FAIL" lines, exit 1 on any FAIL; a hang is caught by the alarm, exit 3):
 *   1. busy_init succeeds; 2. during a 600 ms wait after a loading screen the ticker draws >= 12 frames;
 *   3. once the main thread draws again the ticker draws nothing; 4. 300 rounds of loading screen / random wait /
 *   main frame: no frame mixed from two threads, no mutex error (a wrong-thread unlock, a double lock);
 *   5. the ticker never draws a torn copy of the screen (titles alternate "AAA..." / "BBB..."); 6. the main
 *   thread gets the drawing back within 40 ms. Rounds of both kinds: a main frame between loading screens, and
 *   loading screens back to back (the start, a copy). */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gfx.h"
#include "games.h"
#include "screens.h"
#include "busy.h"

volatile int gx_lent;
void (*gx_take_back)(void);
int gfx_gpu = 1, gfx_dead;
unsigned frame;
int shim_mutex_errors;			/* sys/mutex.h counts LV2 refusals here */

static pthread_mutex_t rec = PTHREAD_MUTEX_INITIALIZER;
static pthread_t owner, main_t;
static int in_frame, mixed, torn;
static volatile int ticker_frames;

#define GX_TAKE()	do { if (gx_lent && gx_take_back) gx_take_back(); } while (0)

static void touch(void)
{
	pthread_t me = pthread_self();

	pthread_mutex_lock(&rec);
	if (!in_frame)
	{
		in_frame = 1;
		owner = me;
	}
	else if (!pthread_equal(owner, me))
		mixed++;
	pthread_mutex_unlock(&rec);
}

static void gx_draw(void)
{
	GX_TAKE();
	touch();
	usleep(300);
}

static void present(void)
{
	GX_TAKE();
	touch();
	usleep(1500);
	pthread_mutex_lock(&rec);
	if (!pthread_equal(owner, main_t))
		ticker_frames++;
	in_frame = 0;
	pthread_mutex_unlock(&rec);
}

void progress_draw(const void *g, const char *head, const char *title, float done, const char *what)
{
	int i, n = (int)strlen(title);
	char c0;

	(void)g; (void)head; (void)done; (void)what;
	for (i = 0; i < 5; i++)
		gx_draw();
	/* read the title slowly: a copy made while it is read shows as a mixed title. Its first letter is read ONCE
	 * (11-28: compared with title[0] read again each time, a copy landing mid-read changed both and passed) */
	c0 = title[0];
	for (i = 1; i < n; i++)
	{
		if (title[i] != c0)
		{
			torn++;
			break;
		}
		if (!pthread_equal(pthread_self(), main_t) && i % 10 == 0)
			usleep(200);
	}
	present();
}

/* as screens.c progress() */
static void progress(const void *g, const char *head, const char *title, float done, const char *what)
{
	busy_save(g, head, title, done, what);
	progress_draw(g, head, title, done, what);
	busy_lend();
}

static void main_frame(void)
{
	int i;

	for (i = 0; i < 4; i++)
		gx_draw();
	present();
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int fails;

static void check(int ok, const char *what)
{
	printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		fails++;
}

static void on_alarm(int s)
{
	(void)s;
	printf("FAIL  hung (a deadlock): the test did not finish in 60 s\n");
	fflush(stdout);
	_exit(3);
}

int main(void)
{
	char ta[151], tb_[151], ln[160];
	game_t g;
	int r, i, before, k;
	double worst = 0.0;

	signal(SIGALRM, on_alarm);
	alarm(60);
	main_t = pthread_self();
	memset(ta, 'A', 150);
	ta[150] = 0;
	memset(tb_, 'B', 150);
	tb_[150] = 0;
	memset(&g, 0, sizeof g);
	srand(1127);

	r = busy_init();
	snprintf(ln, sizeof ln, "busy_init: %d", r);
	check(r == 0, ln);
	if (r)
		return 1;

	progress(&g, "STARTING", ta, 0.3f, "Mounting the game");
	before = ticker_frames;
	usleep(600000);
	snprintf(ln, sizeof ln, "a 600 ms wait after a loading screen: the ticker drew %d frames (12 or more)", ticker_frames - before);
	check(ticker_frames - before >= 12, ln);

	main_frame();
	before = ticker_frames;
	for (i = 0; i < 40; i++)
	{
		main_frame();
		usleep(2000);
	}
	snprintf(ln, sizeof ln, "the main thread drawing again: the ticker drew %d frames (0)", ticker_frames - before);
	check(ticker_frames == before, ln);

	for (k = 0; k < 300; k++)
	{
		double t0;

		progress(&g, "STARTING", (k & 1) ? tb_ : ta, k / 300.0f, "x");
		usleep((useconds_t)(rand() % 80000));
		t0 = now_ms();
		gx_draw();
		if (now_ms() - t0 > worst)
			worst = now_ms() - t0;
		gx_draw();
		present();
	}
	/* loading screens back to back, nothing else drawn between them (the start, a copy): the screen's copy is made
	 * while the ticker may be drawing from it */
	for (k = 0; k < 300; k++)
	{
		progress(&g, "COPYING", (k & 1) ? tb_ : ta, k / 300.0f, "y");
		usleep((useconds_t)(rand() % 30000));
	}
	main_frame();
	snprintf(ln, sizeof ln, "300 + 300 rounds: %d frames mixed from two threads (0); ticker frames in all %d", mixed, ticker_frames);
	check(mixed == 0, ln);
	snprintf(ln, sizeof ln, "LV2 mutex refusals (a wrong-thread unlock, a second lock): %d (0)", shim_mutex_errors);
	check(shim_mutex_errors == 0, ln);
	snprintf(ln, sizeof ln, "torn copies of the screen drawn: %d (0)", torn);
	check(torn == 0, ln);
	snprintf(ln, sizeof ln, "the drawing back to the main thread in at most %.1f ms (40)", worst);
	check(worst <= 40.0, ln);
	printf("---- %s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
