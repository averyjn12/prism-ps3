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
#include "sfx.h"

#define RATE	48000

static float *pcm;
static int n_frames;

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

/* v0.72: the port and its thread are sfx.c's now and stay open for the menu sounds; the opening is its
 * "music". Nothing waits for it to end: intro_sound_tidy() frees it once the mixer has let go of it. */
int intro_sound_play(void)
{
	if (!pcm)
		return -1;
	sound_music(pcm, n_frames);
	return sound_music_done() ? -2 : 0;	/* done at once = there is no port */
}

void intro_sound_end(int quick)
{
	if (quick)
		sound_music_fade();
}

void intro_sound_tidy(void)
{
	if (pcm && sound_music_done())
	{
		free(pcm);
		pcm = NULL;
	}
}
