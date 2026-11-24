/* The opening's sound (v0.30; Bananafuzzbuddy, 11-23: "it'd be cooler with some sort of intro sound").
 * Every sample is computed here from sine and noise: nothing recorded, no Sony sound. Plain C, so the
 * console (intro_sound.c) and the PC (host/introwav.c, which writes a .wav to check) make the same
 * sound. Times are seconds from intro.c's t = 0, each tied to what the picture does then:
 *   0.10-1.05  the beam crosses: a rising "air" sweep of filtered noise, left to centre
 *   0.90       the beam meets the glass: a bell-like ting (A5, inharmonic partials)
 *   1.05-1.55  the colours fan out: six plucked notes up an A major chord, centre to right
 *   1.60-2.08  PRISM draws itself: a soft tap per letter (E4 A4 C#5 E5 A5), panned by letter
 *   1.75-2.20  a quiet rising hiss into
 *   2.20       PLAYSTATION appears: a low soft hit, and a warm A add9 chord swells under the title
 *   2.90       the byline: two small high chimes
 *   5.80-6.50  the chord fades out with the picture (intro.c fades over INTRO_LEN - 0.7 .. INTRO_LEN)
 * A small Schroeder reverb (4 combs + 2 all-pass per side) gives the room; the mix is scaled to a
 * peak of 0.7. */
#include <math.h>
#include <string.h>
#include "intro_synth.h"
#include "intro.h"

#define TAB	4096
#define TWO_PI	6.28318531f

static float sine_t[TAB + 1], soft_t[TAB + 1], pluck_t[TAB + 1];
static int tables_done;

static void tables(void)
{
	int i;

	for (i = 0; i <= TAB; i++)
	{
		float x = (float)i / TAB, s = 0.0f;
		int h;

		sine_t[i] = sinf(TWO_PI * x);
		/* a soft saw: five harmonics, each 0.55 of the one before */
		for (h = 1; h <= 5; h++)
			s += sinf(TWO_PI * h * x) * powf(0.55f, (float)(h - 1)) / h;
		soft_t[i] = s * 0.8f;
		pluck_t[i] = 0.78f * sinf(TWO_PI * x) + 0.22f * sinf(2.0f * TWO_PI * x) + 0.08f * sinf(3.0f * TWO_PI * x);
	}
	tables_done = 1;
}

static inline float look(const float *t, float ph)
{
	float x = ph * TAB;
	int i = (int)x;

	return t[i] + (t[i + 1] - t[i]) * (x - (float)i);
}

static float clamp01(float x)
{
	return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

static float smooth(float t, float a, float b)
{
	float x = clamp01((t - a) / (b - a));

	return x * x * (3.0f - 2.0f * x);
}

static unsigned seed = 0x2468ACE1u;

static float noise(void)
{
	seed = seed * 1664525u + 1013904223u;
	return (float)((seed >> 9) & 0xFFFF) / 32768.0f - 1.0f;
}

/* one decaying tone: attack atk s, then e^(-t/tau); pan -1 left .. +1 right */
static void tone(float *out, int n, int sr, float t0, float f, float amp, float atk, float tau, float pan, const float *tab)
{
	int s0 = (int)(t0 * sr), na = (int)(atk * sr) + 1, s1 = s0 + na + (int)(tau * 7.0f * sr), i;
	float ph = 0.0f, dph = f / sr, e = 1.0f, d = expf(-1.0f / (tau * sr));
	float gl = amp * sqrtf(0.5f * (1.0f - pan)), gr = amp * sqrtf(0.5f * (1.0f + pan));

	if (s1 > n)
		s1 = n;
	for (i = s0 < 0 ? 0 : s0; i < s1; i++)
	{
		int k = i - s0;
		float v;

		if (k >= na)
			e *= d;
		v = look(tab, ph) * e * (k < na ? (float)k / na : 1.0f);
		ph += dph;
		if (ph >= 1.0f)
			ph -= 1.0f;
		out[2 * i] += v * gl;
		out[2 * i + 1] += v * gr;
	}
}

/* noise through a state-variable band-pass whose centre moves f0 -> f1 between t0 and t1 */
static void sweep(float *out, int n, int sr, float t0, float t1, float f0, float f1, float q, float amp,
	float ea, float eb, float ec, float ed, float pan0, float pan1)
{
	int s0 = (int)(t0 * sr), s1 = (int)(t1 * sr), i;
	float lp = 0.0f, bp = 0.0f, g = 0.0f;

	if (s1 > n)
		s1 = n;
	for (i = s0; i < s1; i++)
	{
		float t = (float)i / sr, p = clamp01((t - t0) / (t1 - t0)), hp, env, pan, v;

		if (((i - s0) & 31) == 0)
			g = 2.0f * sinf(3.14159265f * (f0 * powf(f1 / f0, p)) / sr);
		lp += g * bp;
		hp = noise() - lp - q * bp;
		bp += g * hp;
		env = smooth(t, ea, eb) * (1.0f - smooth(t, ec, ed));
		pan = pan0 + (pan1 - pan0) * p;
		v = bp * env * amp;
		out[2 * i] += v * sqrtf(0.5f * (1.0f - pan));
		out[2 * i + 1] += v * sqrtf(0.5f * (1.0f + pan));
	}
}

/* the low hit: a sine falling from 95 to 45 Hz, ~0.55 s */
static void hit(float *out, int n, int sr, float t0, float amp)
{
	int s0 = (int)(t0 * sr), s1 = s0 + (int)(3.0f * sr), i;
	float ph = 0.0f;

	if (s1 > n)
		s1 = n;
	for (i = s0; i < s1; i++)
	{
		float k = (float)(i - s0) / sr, f = 45.0f + 50.0f * expf(-k / 0.08f);
		float env = (k < 0.006f ? k / 0.006f : expf(-k / 0.55f)), v;

		ph += f / sr;
		if (ph >= 1.0f)
			ph -= 1.0f;
		v = look(sine_t, ph) * env * amp;
		out[2 * i] += v;
		out[2 * i + 1] += v;
	}
}

/* the chord under the title: each note two soft saws a little apart per side, through a low-pass that
 * opens as it swells; it fades with the picture */
static void pad(float *out, int n, int sr, float t0)
{
	static const float notes[] = { 110.0f, 164.81f, 220.0f, 277.18f, 329.63f, 493.88f };
	static const float gain[] = { 0.050f, 0.040f, 0.036f, 0.032f, 0.028f, 0.022f };
	float ph[6][4], yl = 0.0f, yr = 0.0f;
	int s0 = (int)(t0 * sr), i, k;

	memset(ph, 0, sizeof ph);
	for (i = s0; i < n; i++)
	{
		float t = (float)i / sr, env = smooth(t, t0, t0 + 1.3f) * (1.0f - smooth(t, INTRO_LEN - 0.75f, INTRO_LEN + 0.05f));
		float l = 0.0f, r = 0.0f, fc, c;

		if (env <= 0.0f && t > t0 + 1.0f)
			break;
		for (k = 0; k < 6; k++)
		{
			static const float det[4] = { -0.0034f, 0.0012f, 0.0034f, -0.0012f };
			int j;

			for (j = 0; j < 4; j++)
			{
				ph[k][j] += notes[k] * (1.0f + det[j]) / sr;
				if (ph[k][j] >= 1.0f)
					ph[k][j] -= 1.0f;
			}
			l += (look(soft_t, ph[k][0]) + look(soft_t, ph[k][1])) * gain[k];
			r += (look(soft_t, ph[k][2]) + look(soft_t, ph[k][3])) * gain[k];
		}
		fc = 700.0f + 1900.0f * smooth(t, t0, t0 + 1.8f);
		c = 1.0f - expf(-TWO_PI * fc / sr);
		yl += c * (l - yl);
		yr += c * (r - yr);
		out[2 * i] += yl * env;
		out[2 * i + 1] += yr * env;
	}
}

/* ---- room: Schroeder reverb, Freeverb's delay lengths scaled to the rate ---- */
#define NCOMB	4
#define NAP	2
#define RMAX	2048

static float cbuf[2][NCOMB][RMAX], abuf[2][NAP][RMAX];

static void room(float *out, int n, int sr, float wet)
{
	static const int cl[NCOMB] = { 1116, 1188, 1277, 1356 }, al[NAP] = { 556, 441 };
	int clen[2][NCOMB], alen[2][NAP], ci[2][NCOMB], ai[2][NAP], i, c, k;
	float store[2][NCOMB];

	memset(cbuf, 0, sizeof cbuf);
	memset(abuf, 0, sizeof abuf);
	memset(ci, 0, sizeof ci);
	memset(ai, 0, sizeof ai);
	memset(store, 0, sizeof store);
	for (c = 0; c < 2; c++)
	{
		for (k = 0; k < NCOMB; k++)
			clen[c][k] = (cl[k] + c * 23) * sr / 44100;
		for (k = 0; k < NAP; k++)
			alen[c][k] = (al[k] + c * 23) * sr / 44100;
	}
	for (i = 0; i < n; i++)
		for (c = 0; c < 2; c++)
		{
			float x = out[2 * i + c] * 0.25f, y = 0.0f;

			for (k = 0; k < NCOMB; k++)
			{
				float o = cbuf[c][k][ci[c][k]];

				store[c][k] = o * 0.72f + store[c][k] * 0.28f;	/* damping */
				cbuf[c][k][ci[c][k]] = x + store[c][k] * 0.84f;	/* room size */
				if (++ci[c][k] >= clen[c][k])
					ci[c][k] = 0;
				y += o;
			}
			for (k = 0; k < NAP; k++)
			{
				float b = abuf[c][k][ai[c][k]];

				abuf[c][k][ai[c][k]] = y + b * 0.5f;
				y = b - y;
				if (++ai[c][k] >= alen[c][k])
					ai[c][k] = 0;
			}
			out[2 * i + c] += y * wet;
		}
}

int intro_synth_frames(int rate)
{
	return (int)((INTRO_LEN + 0.35f) * rate);
}

void intro_synth(float *out, int n, int sr)
{
	static const float bell_r[] = { 1.0f, 2.0f, 2.76f, 5.40f, 8.93f };
	static const float bell_a[] = { 0.17f, 0.05f, 0.085f, 0.035f, 0.018f };
	static const float bell_t[] = { 1.8f, 1.0f, 0.8f, 0.35f, 0.2f };
	static const float arp[] = { 440.0f, 554.37f, 659.26f, 880.0f, 1108.73f, 1318.51f };
	static const float taps[] = { 329.63f, 440.0f, 554.37f, 659.26f, 880.0f };
	float peak = 0.0f;
	int i, k;

	if (!tables_done)
		tables();
	seed = 0x2468ACE1u;
	memset(out, 0, sizeof(float) * 2 * (size_t)n);

	/* the beam crosses */
	sweep(out, n, sr, 0.05f, 1.15f, 350.0f, 3200.0f, 0.45f, 0.55f, 0.08f, 0.85f, 0.88f, 1.10f, -0.8f, 0.0f);
	/* it meets the glass */
	for (k = 0; k < 5; k++)
		tone(out, n, sr, 0.90f, 880.0f * bell_r[k], bell_a[k], 0.003f, bell_t[k], 0.0f, sine_t);
	/* the colours fan out */
	for (k = 0; k < 6; k++)
		tone(out, n, sr, 1.05f + 0.10f * k, arp[k], 0.10f, 0.004f, 0.45f, -0.1f + 0.18f * k, pluck_t);
	/* the letters */
	for (k = 0; k < 5; k++)
	{
		float pan = -0.35f + 0.175f * k;

		tone(out, n, sr, 1.60f + 0.12f * k, taps[k], 0.07f, 0.003f, 0.30f, pan, sine_t);
		tone(out, n, sr, 1.60f + 0.12f * k, taps[k] * 4.0f, 0.018f, 0.002f, 0.07f, pan, sine_t);
	}
	/* the rise into PLAYSTATION, the hit, the chord */
	sweep(out, n, sr, 1.70f, 2.22f, 2500.0f, 7000.0f, 0.6f, 0.16f, 1.75f, 2.18f, 2.19f, 2.22f, 0.0f, 0.0f);
	hit(out, n, sr, 2.20f, 0.42f);
	pad(out, n, sr, 2.15f);
	/* the byline */
	tone(out, n, sr, 2.90f, 1318.51f, 0.045f, 0.004f, 0.9f, -0.3f, sine_t);
	tone(out, n, sr, 3.02f, 1975.53f, 0.035f, 0.004f, 0.8f, 0.3f, sine_t);

	room(out, n, sr, 0.30f);

	for (i = 0; i < 2 * n; i++)
		if (fabsf(out[i]) > peak)
			peak = fabsf(out[i]);
	if (peak > 0.0f)
	{
		float g = 0.7f / peak;

		for (i = 0; i < 2 * n; i++)
			out[i] *= g;
	}
	/* the last 50 ms to silence, so nothing clicks when the port stops */
	for (i = n - sr / 20; i < n; i++)
	{
		float g = (float)(n - i) / (sr / 20);

		out[2 * i] *= g;
		out[2 * i + 1] *= g;
	}
}
