/* v0.72: src/sfx.c (the sound mixer) on the PC, over a pretend audio port (host/fake_audio/).
 *   bash host/sfx_test.sh      -> "sfx_test: N checks, 0 failed"
 * The port here is a ring of 8 blocks of 256 stereo frames and a thread that "plays" one block every
 * 5.33 ms, as the console's does: it copies the block out to a tape, wipes it, and moves on. The test
 * then reads the tape: when a menu sound was heard after sfx() was called, how long it lasted, how loud,
 * whether the opening's music came out whole and in order, and whether anything was written twice or
 * too far ahead. It is a stand-in: the console's port may differ in ways this does not know. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include "sfx.h"

#define NB		8
#define TAPE_BLOCKS	4000

static float ring[NB * AUDIO_BLOCK_SAMPLES * 2];
static volatile u64 read_index;
static float *tape;
static volatile int tape_blocks, port_run;
static pthread_t port_th;
static int checks, failed;

static void check(int ok, const char *what, double a, double b)
{
	checks++;
	if (!ok)
	{
		failed++;
		printf("FAIL: %s (%g, %g)\n", what, a, b);
	}
}

static void nap(double ms)
{
	struct timespec t = { (time_t)(ms / 1000.0), (long)(fmod(ms, 1000.0) * 1e6) };

	nanosleep(&t, NULL);
}

static void *port_thread(void *arg)
{
	(void)arg;
	while (port_run)
	{
		u64 b = read_index;

		/* the block is taken as it is when its turn STARTS (the worst case): what is written into it while it
		 * plays is not heard, so the mixer has to be done with a block before the port gets to it */
		if (tape_blocks < TAPE_BLOCKS)
			memcpy(tape + (size_t)tape_blocks * AUDIO_BLOCK_SAMPLES * 2, ring + b * AUDIO_BLOCK_SAMPLES * 2, sizeof(float) * AUDIO_BLOCK_SAMPLES * 2);
		nap(256000.0 / 48000.0);
		tape_blocks++;
		/* a block left alone after this is played again as it is: wipe it with a mark, so stale or doubled
		 * blocks show on the tape */
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES * 2; i++)
			ring[b * AUDIO_BLOCK_SAMPLES * 2 + i] = 9.0f;
		read_index = (b + 1) % NB;
	}
	return NULL;
}

s32 audioInit(void) { return 0; }
s32 audioQuit(void) { return 0; }
s32 audioPortOpen(audioPortParam *p, u32 *n) { *n = 1; return p->numBlocks == NB && p->numChannels == AUDIO_PORT_2CH ? 0 : -1; }
s32 audioGetPortConfig(u32 n, audioPortConfig *c)
{
	(void)n;
	memset(c, 0, sizeof *c);
	c->readIndex = (u64)(uintptr_t)&read_index;
	c->numBlocks = NB;
	c->channelCount = 2;
	c->audioDataStart = (u64)(uintptr_t)ring;
	return 0;
}
s32 audioPortStart(u32 n) { (void)n; port_run = 1; return pthread_create(&port_th, NULL, port_thread, NULL); }
s32 audioPortStop(u32 n) { (void)n; port_run = 0; pthread_join(port_th, NULL); return 0; }
s32 audioPortClose(u32 n) { (void)n; return 0; }

struct start { void (*fn)(void *); void *arg; };
static void *tramp(void *p) { struct start s = *(struct start *)p; free(p); s.fn(s.arg); return NULL; }
s32 sysThreadCreate(sys_ppu_thread_t *th, void (*entry)(void *), void *arg, s32 prio, u64 stack, u64 flags, char *name)
{
	struct start *s = malloc(sizeof *s);

	(void)prio; (void)stack; (void)flags; (void)name;
	s->fn = entry;
	s->arg = arg;
	return pthread_create(th, NULL, tramp, s);
}
s32 sysThreadJoin(sys_ppu_thread_t th, u64 *rv) { (void)rv; return pthread_join(th, NULL); }
void sysThreadExit(u64 v) { (void)v; pthread_exit(NULL); }

/* the tape from frame a to frame b: the first frame louder than eps, the last, the peak, marks, NaNs */
static void scan(int a, int b, float eps, int *first, int *last, float *peak, int *marks, int *bad)
{
	int i;

	*first = *last = -1;
	*peak = 0.0f;
	*marks = *bad = 0;
	for (i = a; i < b; i++)
	{
		float l = tape[2 * i], r = tape[2 * i + 1], m = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);

		if (l != l || r != r)
			(*bad)++;
		if (l == 9.0f || r == 9.0f)
		{
			(*marks)++;
			continue;
		}
		if (m > eps)
		{
			if (*first < 0)
				*first = i;
			*last = i;
		}
		if (m > *peak)
			*peak = m;
	}
}

int main(void)
{
	int first, last, marks, bad, at, i, k;
	float peak, *music;
	const int MN = 48000 / 2;

	tape = calloc((size_t)TAPE_BLOCKS * AUDIO_BLOCK_SAMPLES * 2, sizeof(float));
	check(sound_open() == 0, "sound_open", 0, 0);
	check(sound_open() == 0, "a second sound_open is harmless", 0, 0);
	nap(120);

	/* 1. silence, and never a wiped or doubled block: the mixer keeps in front of the port */
	at = tape_blocks * AUDIO_BLOCK_SAMPLES;
	nap(200);
	scan(8 * AUDIO_BLOCK_SAMPLES, tape_blocks * AUDIO_BLOCK_SAMPLES, 1e-6f, &first, &last, &peak, &marks, &bad);
	check(first < 0 && peak == 0.0f, "silence before any sound", first, peak);
	check(marks == 0, "a block was played that the mixer had not written (it fell behind the port)", marks, 0);
	check(bad == 0, "NaN on the tape", bad, 0);

	/* 2. each menu sound: heard soon after the call, about as long as it should be, not loud, then silence */
	for (k = 0; k < SFX_N; k++)
	{
		static const double want_ms[SFX_N] = { 60, 200, 200 };
		double delay, len;

		nap(60);
		at = tape_blocks * AUDIO_BLOCK_SAMPLES;
		sfx(k);
		nap(want_ms[k] + 150);
		scan(at, tape_blocks * AUDIO_BLOCK_SAMPLES, 0.002f, &first, &last, &peak, &marks, &bad);
		delay = first < 0 ? 1e9 : (first - at) / 48.0;
		len = first < 0 ? 0 : (last - first) / 48.0;
		printf("   sound %d: heard %.1f ms after the call, for %.0f ms, peak %.3f\n", k, delay, len, peak);
		check(first >= 0, "a menu sound was never heard", k, 0);
		check(delay <= 30.0, "a menu sound came more than 30 ms after the press", k, delay);
		check(len >= want_ms[k] * 0.35 && len <= want_ms[k] + 6, "a menu sound's length", k, len);
		check(peak >= 0.05f && peak <= 0.35f, "a menu sound's loudness", k, peak);
		check(marks == 0 && bad == 0, "a missed block or NaN during a menu sound", marks, bad);
		scan(last + 480, tape_blocks * AUDIO_BLOCK_SAMPLES, 1e-6f, &first, &last, &peak, &marks, &bad);
		check(first < 0, "sound went on after a menu sound's end", k, first);
	}

	/* 3. quick presses overlap instead of cutting each other: ten ticks 30 ms apart */
	nap(60);
	at = tape_blocks * AUDIO_BLOCK_SAMPLES;
	for (i = 0; i < 10; i++)
	{
		sfx(SFX_MOVE);
		nap(30);
	}
	nap(150);
	scan(at, tape_blocks * AUDIO_BLOCK_SAMPLES, 0.002f, &first, &last, &peak, &marks, &bad);
	check(first >= 0 && (last - first) / 48.0 > 250.0, "ten quick ticks did not all sound", (last - first) / 48.0, 0);
	check(peak <= 0.7f, "overlapping ticks clip", peak, 0);
	check(marks == 0 && bad == 0, "a missed block or NaN during quick ticks", marks, bad);

	/* 4. the opening's music: half a second of a ramp, out whole and in order, a tick mixed on top */
	music = malloc(sizeof(float) * 2 * MN);
	for (i = 0; i < MN; i++)
	{
		music[2 * i] = 0.1f + 0.2f * (float)i / MN;
		music[2 * i + 1] = 0.5f * music[2 * i];	/* left and right differ: a swap shows */
	}
	nap(60);
	at = tape_blocks * AUDIO_BLOCK_SAMPLES;
	check(sound_music_done(), "music done before any music", 0, 0);
	sound_music(music, MN);
	check(!sound_music_done(), "music not done right after it starts", 0, 0);
	nap(700);
	check(sound_music_done(), "music done after it has played", 0, 0);
	scan(at, tape_blocks * AUDIO_BLOCK_SAMPLES, 0.05f, &first, &last, &peak, &marks, &bad);
	check(first >= 0 && last - first == MN - 1, "the music's length on the tape", last - first + 1, MN);
	if (first >= 0)
	{
		int wrong = 0;

		for (i = 0; i < MN && first + i < tape_blocks * AUDIO_BLOCK_SAMPLES; i++)
			if (fabsf(tape[2 * (first + i)] - music[2 * i]) > 1e-5f || fabsf(tape[2 * (first + i) + 1] - music[2 * i + 1]) > 1e-5f)
				wrong++;
		check(wrong == 0, "frames of the music that came out changed or out of order", wrong, MN);
	}
	check(marks == 0 && bad == 0, "a missed block or NaN during the music", marks, bad);

	/* 5. a skipped opening fades out within 150 ms and is then done */
	for (i = 0; i < MN; i++)
		music[2 * i] = music[2 * i + 1] = 0.3f;
	nap(60);
	sound_music(music, MN);
	nap(100);
	at = tape_blocks * AUDIO_BLOCK_SAMPLES;
	sound_music_fade();
	nap(300);
	check(sound_music_done(), "music done after a fade", 0, 0);
	scan(at, tape_blocks * AUDIO_BLOCK_SAMPLES, 0.001f, &first, &last, &peak, &marks, &bad);
	check(last >= 0 && (last - at) / 48.0 <= 200.0, "a fade took longer than 150 ms (+ what was already written)", (last - at) / 48.0, 0);
	check(peak <= 0.3001f, "a fade got louder", peak, 0);

	/* 6. closing stops everything; sfx() after that does nothing and does not crash */
	sound_close();
	sfx(SFX_OK);
	sound_music(music, MN);
	check(sound_music_done(), "music 'done' with the port closed", 0, 0);
	check(sound_open() == 0, "open again after a close", 0, 0);
	nap(60);
	at = tape_blocks * AUDIO_BLOCK_SAMPLES;
	sfx(SFX_OK);
	nap(300);
	scan(at, tape_blocks * AUDIO_BLOCK_SAMPLES, 0.002f, &first, &last, &peak, &marks, &bad);
	check(first >= 0, "no sound after opening again", 0, 0);
	sound_close();
	printf("sfx_test: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
