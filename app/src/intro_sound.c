/* v0.30: the opening's sound on the console. intro_synth.c computes it into memory before the intro
 * (during the black lead-in); a thread then copies it into the audio port's ring of 256-sample blocks,
 * always filling up to the block the port is reading (PSL1GHT samples/audio: config.readIndex is the
 * address of a 64-bit block counter). A skipped intro fades out in 150 ms. 48 kHz stereo float, the
 * only format the port plays. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include <lv2/sysfs.h>
#include "intro_synth.h"
#include "intro_sound.h"

#define RATE	48000

static float *pcm;
static int n_frames, pos, opened;
static volatile int running, fading;
static u32 port;
static audioPortConfig cfg;
static sys_ppu_thread_t th;

/* v0.32: computing it took 1.2 s on the console (prism_intro.log 11-23), all of it before the intro
 * could start; the package now carries it, rendered on the PC by the same code (host/introwav.c,
 * build_app.sh): USRDIR/intro.wav, 16-bit stereo 48 kHz. Computed here only if that file is missing
 * or not the expected size. 1 = loaded, 0 = computed, negative = no sound. */
#define INTRO_WAV	"/dev_hdd0/game/PRSM00001/USRDIR/intro.wav"

int intro_sound_prepare(void)
{
	sysFSStat st;
	s32 fd;

	n_frames = intro_synth_frames(RATE);
	pcm = (float *)malloc(sizeof(float) * 2 * (size_t)n_frames);
	if (!pcm)
		return -1;
	/* straight lv2 reads: PSL1GHT's stdio would cut them into 1 KB calls (rip.c) */
	if (sysFsStat(INTRO_WAV, &st) == 0 && st.st_size == 44 + (u64)n_frames * 4
		&& sysFsOpen(INTRO_WAV, SYS_O_RDONLY, &fd, NULL, 0) == 0)
	{
		static u8 raw[64 * 1024];
		u64 pos0 = 0;
		int i = 0, k;

		sysFsLseek(fd, 44, 0, &pos0);
		while (i < 2 * n_frames)
		{
			u64 n = 0;

			if (sysFsRead(fd, raw, sizeof raw, &n) != 0 || n == 0)
				break;
			for (k = 0; k + 1 < (int)n && i < 2 * n_frames; k += 2)
				pcm[i++] = (float)(s16)(raw[k] | raw[k + 1] << 8) / 32768.0f;
		}
		sysFsClose(fd);
		if (i == 2 * n_frames)
			return 1;
	}
	intro_synth(pcm, n_frames, RATE);
	return 0;
}

static void feeder(void *arg)
{
	volatile u64 *ri = (volatile u64 *)(u64)cfg.readIndex;
	float *base = (float *)(u64)cfg.audioDataStart, g = 1.0f;
	u64 nb = cfg.numBlocks, next = (*ri + 1) % nb;
	int after = 0, i;

	(void)arg;
	while (running)
	{
		u64 cur = *ri;

		while (next != cur)
		{
			float *b = base + next * 2 * AUDIO_BLOCK_SAMPLES;

			for (i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
			{
				float l = 0.0f, r = 0.0f;

				if (pos < n_frames)
				{
					l = pcm[2 * pos];
					r = pcm[2 * pos + 1];
					pos++;
				}
				if (fading && g > 0.0f)
					g -= 1.0f / (RATE * 0.15f);
				if (g < 0.0f)
					g = 0.0f;
				b[2 * i] = l * g;
				b[2 * i + 1] = r * g;
			}
			if (pos >= n_frames || g <= 0.0f)
				after++;
			next = (next + 1) % nb;
		}
		if (after > (int)nb + 2)
			break;		/* every real block has been played */
		usleep(2000);
	}
	sysThreadExit(0);
}

int intro_sound_play(void)
{
	audioPortParam p;

	if (!pcm)
		return -1;
	sysModuleLoad(SYSMODULE_AUDIO);
	if (audioInit() != 0)
		return -2;
	memset(&p, 0, sizeof p);
	p.numChannels = AUDIO_PORT_2CH;
	p.numBlocks = AUDIO_BLOCK_16;
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
	opened = 1;
	if (sysThreadCreate(&th, feeder, NULL, 500, 0x4000, THREAD_JOINABLE, (char *)"prism_intro_sound") != 0)
	{
		running = 0;
		audioPortStop(port);
		audioPortClose(port);
		audioQuit();
		opened = 0;
		return -5;
	}
	return 0;
}

void intro_sound_end(int quick)
{
	u64 rv;

	if (opened)
	{
		if (quick)
			fading = 1;
		sysThreadJoin(th, &rv);
		running = 0;
		audioPortStop(port);
		audioPortClose(port);
		audioQuit();
		opened = 0;
	}
	free(pcm);
	pcm = NULL;
}
