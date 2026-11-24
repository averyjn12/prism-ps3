/* v0.30: write the opening's sound (src/intro_synth.c) to a 16-bit stereo .wav, to check and hear
 * it on the PC:  introwav <out.wav> */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "intro_synth.h"

static void w32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

int main(int argc, char **argv)
{
	const int sr = 48000;
	int n = intro_synth_frames(sr), i;
	float *b = malloc(sizeof(float) * 2 * n);
	FILE *f;

	if (argc < 2 || !b)
		return 1;
	intro_synth(b, n, sr);
	f = fopen(argv[1], "wb");
	if (!f)
		return 2;
	fwrite("RIFF", 1, 4, f); w32(f, 36 + n * 4); fwrite("WAVEfmt ", 1, 8, f);
	w32(f, 16); w16(f, 1); w16(f, 2); w32(f, sr); w32(f, sr * 4); w16(f, 4); w16(f, 16);
	fwrite("data", 1, 4, f); w32(f, n * 4);
	for (i = 0; i < 2 * n; i++)
	{
		float x = b[i] < -1.0f ? -1.0f : b[i] > 1.0f ? 1.0f : b[i];

		w16(f, (unsigned)(int16_t)(x * 32767.0f) & 0xFFFF);
	}
	fclose(f);
	printf("%d frames, %.2f s\n", n, (double)n / sr);
	return 0;
}
