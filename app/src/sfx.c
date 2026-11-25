/* v0.72: the audio port and its mixer (sfx.h). Console only.
 *
 * The port is the one intro_sound.c opened for the opening since v0.30 (PSL1GHT samples/audio: 48 kHz
 * stereo float in a ring of 256-sample blocks, config.readIndex the address of a 64-bit block counter).
 * New: it stays open, and the thread writes only AHEAD blocks in front of the one being played instead
 * of the whole ring, so a menu sound is heard about 20 ms after the press, not 85. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include "sfx.h"

#define RATE	48000
#define AHEAD	3		/* blocks written in front of the playing one: 16 ms */
#define VOICES	4
#define TWO_PI	6.28318531f

static int menu_on = 1;

int menu_sounds_on(void)
{
	return menu_on;
}

int menu_sounds_set(int on)
{
	FILE *f;

	menu_on = on != 0;
	if (on)
	{
		remove(SFX_OFF_FILE);
		f = fopen(SFX_OFF_FILE, "rb");
		if (f)
		{
			fclose(f);
			return -1;
		}
		return 0;
	}
	f = fopen(SFX_OFF_FILE, "wb");
	if (!f)
		return -1;
	fputs("off\n", f);
	return fclose(f) ? -1 : 0;
}

static u32 port;
static audioPortConfig cfg;
static sys_ppu_thread_t th;
static int opened;
static volatile int running;

/* the menu sounds, computed once */
static float *snd[SFX_N];
static int snd_n[SFX_N];

static struct
{
	const float *buf;
	int n;
	volatile int pos;	/* pos >= n: free */
} voice[VOICES];
static int next_voice;

static const float *music;
static volatile int music_n, music_pos, music_fading;
static float music_gain = 1.0f;

/* one soft note: a sine and a quieter octave above it, 2 ms in, dying away with time constant tau */
static void note(float *out, int n, float t0, float f, float amp, float tau)
{
	int i, i0 = (int)(t0 * RATE);

	for (i = i0; i < n; i++)
	{
		float t = (float)(i - i0) / RATE, e = expf(-t / tau) * (t < 0.002f ? t / 0.002f : 1.0f);
		float v = amp * e * (sinf(TWO_PI * f * t) + 0.25f * sinf(TWO_PI * 2.0f * f * t));

		out[2 * i] += v;
		out[2 * i + 1] += v;
	}
}

static int make(int which, float secs)
{
	int n = (int)(secs * RATE), i;

	snd[which] = (float *)calloc((size_t)n * 2, sizeof(float));
	if (!snd[which])
		return -1;
	snd_n[which] = n;
	if (which == SFX_MOVE)
		note(snd[which], n, 0.0f, 1318.5f, 0.16f, 0.012f);		/* E6, a short tick */
	else if (which == SFX_OK)
	{
		note(snd[which], n, 0.0f, 880.0f, 0.15f, 0.030f);		/* A5 up to E6 */
		note(snd[which], n, 0.055f, 1318.5f, 0.15f, 0.045f);
	}
	else
	{
		note(snd[which], n, 0.0f, 1318.5f, 0.12f, 0.030f);		/* E6 down to A5, quieter */
		note(snd[which], n, 0.055f, 880.0f, 0.12f, 0.045f);
	}
	for (i = n - 96; i < n; i++)	/* the last 2 ms run out to nothing: no click */
	{
		float g = (float)(n - i) / 96.0f;

		if (i < 0)
			continue;
		snd[which][2 * i] *= g;
		snd[which][2 * i + 1] *= g;
	}
	return 0;
}

static void mixer(void *arg)
{
	volatile u64 *ri = (volatile u64 *)(u64)cfg.readIndex;
	float *base = (float *)(u64)cfg.audioDataStart;
	u64 nb = cfg.numBlocks, next = (*ri + 1) % nb, ahead = AHEAD + 3 <= nb ? AHEAD : (nb > 3 ? nb - 3 : 1);	/* never the whole ring */
	int i, k;

	(void)arg;
	while (running)
	{
		u64 cur = *ri;

		if ((next + nb - cur) % nb > ahead + 2)
			next = (cur + 1) % nb;		/* fell behind the port (a long file read on the main thread): catch up */
		while ((next + nb - cur) % nb <= ahead)
		{
			float *b = base + next * 2 * AUDIO_BLOCK_SAMPLES;

			memset(b, 0, sizeof(float) * 2 * AUDIO_BLOCK_SAMPLES);
			if (music)
			{
				int p = music_pos, n = music_n;

				for (i = 0; i < AUDIO_BLOCK_SAMPLES && p < n && music_gain > 0.0f; i++, p++)
				{
					if (music_fading)
						music_gain -= 1.0f / (RATE * 0.15f);
					if (music_gain < 0.0f)
						music_gain = 0.0f;
					b[2 * i] += music[2 * p] * music_gain;
					b[2 * i + 1] += music[2 * p + 1] * music_gain;
				}
				music_pos = (p >= n || music_gain <= 0.0f) ? n : p;
			}
			for (k = 0; k < VOICES; k++)
			{
				int p = voice[k].pos, n = voice[k].n;
				const float *s = voice[k].buf;

				if (!s || p >= n)
					continue;
				for (i = 0; i < AUDIO_BLOCK_SAMPLES && p < n; i++, p++)
				{
					b[2 * i] += s[2 * p];
					b[2 * i + 1] += s[2 * p + 1];
				}
				voice[k].pos = p;
			}
			next = (next + 1) % nb;
		}
		usleep(2000);
	}
	sysThreadExit(0);
}

int sound_open(void)
{
	audioPortParam p;
	int k;

	if (opened)
		return 0;
	{
		FILE *f = fopen(SFX_OFF_FILE, "rb");

		menu_on = f == NULL;
		if (f)
			fclose(f);
	}
	if (make(SFX_MOVE, 0.060f) || make(SFX_OK, 0.200f) || make(SFX_BACK, 0.200f))
		return -1;
	for (k = 0; k < VOICES; k++)
		voice[k].pos = voice[k].n = 0;
	sysModuleLoad(SYSMODULE_AUDIO);
	if (audioInit() != 0)
		return -2;
	memset(&p, 0, sizeof p);
	p.numChannels = AUDIO_PORT_2CH;
	p.numBlocks = AUDIO_BLOCK_8;
	p.attrib = AUDIO_PORT_INITLEVEL;
	p.level = 1.0f;
	if (audioPortOpen(&p, &port) != 0)
	{
		audioQuit();
		return -3;
	}
	if (audioGetPortConfig(port, &cfg) != 0 || audioPortStart(port) != 0)
	{
		audioPortClose(port);
		audioQuit();
		return -4;
	}
	running = 1;
	if (sysThreadCreate(&th, mixer, NULL, 500, 0x4000, THREAD_JOINABLE, (char *)"prism_sound") != 0)
	{
		running = 0;
		audioPortStop(port);
		audioPortClose(port);
		audioQuit();
		return -5;
	}
	opened = 1;
	return 0;
}

void sound_close(void)
{
	u64 rv;

	if (!opened)
		return;
	running = 0;
	sysThreadJoin(th, &rv);
	audioPortStop(port);
	audioPortClose(port);
	audioQuit();
	opened = 0;
	music = NULL;
}

void sfx(int which)
{
	int k;

	if (!opened || !menu_on || which < 0 || which >= SFX_N || !snd[which])
		return;
	k = next_voice;
	next_voice = (next_voice + 1) % VOICES;
	voice[k].pos = 0x7FFFFFFF;	/* silent while its fields change */
	voice[k].buf = snd[which];
	voice[k].n = snd_n[which];
	voice[k].pos = 0;
}

void sound_music(const float *pcm, int n)
{
	if (!opened)
		return;
	music = NULL;
	music_gain = 1.0f;
	music_fading = 0;
	music_pos = 0;
	music_n = n;
	music = pcm;
}

void sound_music_fade(void)
{
	music_fading = 1;
}

int sound_music_done(void)
{
	return !opened || !music || music_pos >= music_n;
}
